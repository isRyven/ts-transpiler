#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>

#include <quickjs.h>
#include <un7z.h>

#include "libstd.h"

#define PROG_NAME "tss"
#if defined(_WIN32)
#define OS_PLATFORM "win32"
#elif defined(__APPLE__)
#define OS_PLATFORM "darwin"
#else
#define OS_PLATFORM "linux"
#endif

#define MANIFEST_FILE "manifest.json"

/* <<<<<<<<<<<<<<<<<< VFS >>>>>>>>>>>>>>>>>> */

extern const unsigned char pak_data[];
extern const unsigned int  pak_data_length;

typedef struct un7z_t { 
    CSzArEx db; 
    CLookToRead stream; 
    /* cache results */
    UInt32 blockIndex;
    Byte *outBuffer;
    size_t outBufferSize;
} un7z_t; 

un7z_t un7z_ctx;

static Byte kUtf8Limits[5] = { 0xC0, 0xE0, 0xF0, 0xF8, 0xFC };

static Bool Utf16Le_To_Utf8(Byte *dest, size_t *destLen, const Byte *srcUtf16Le, size_t srcUtf16LeLen)
{
  size_t destPos = 0;
  const Byte *srcUtf16LeEnd = srcUtf16Le + srcUtf16LeLen * 2;
  for (;;)
  {
    unsigned numAdds;
    UInt32 value;
    if (srcUtf16Le == srcUtf16LeEnd)
    {
      *destLen = destPos;
      return True;
    }
    value = GetUi16(srcUtf16Le);
    srcUtf16Le += 2;
    if (value < 0x80)
    {
      if (dest)
        dest[destPos] = (char)value;
      destPos++;
      continue;
    }
    if (value >= 0xD800 && value < 0xE000)
    {
      UInt32 c2;
      if (value >= 0xDC00 || srcUtf16Le == srcUtf16LeEnd)
        break;
      c2 = GetUi16(srcUtf16Le);
      srcUtf16Le += 2;
      if (c2 < 0xDC00 || c2 >= 0xE000)
        break;
      value = (((value - 0xD800) << 10) | (c2 - 0xDC00)) + 0x10000;
    }
    for (numAdds = 1; numAdds < 5; numAdds++)
      if (value < (((UInt32)1) << (numAdds * 5 + 6)))
        break;
    if (dest)
      dest[destPos] = (char)(kUtf8Limits[numAdds - 1] + (value >> (6 * numAdds)));
    destPos++;
    do
    {
      numAdds--;
      if (dest)
        dest[destPos] = (char)(0x80 + ((value >> (6 * numAdds)) & 0x3F));
      destPos++;
    }
    while (numAdds != 0);
  }
  *destLen = destPos;
  return False;
}

UInt32 un7z_find(un7z_t *ctx, const char *path, UInt32 *fileIndex)
{
    SRes res = SZ_OK;
    UInt32 i;
    Byte *filename_utf8 = NULL;
    size_t filename_utf8_capacity = 0;

    *fileIndex = 0xFFFFFFFF;

    for (i = 0; i < ctx->db.db.NumFiles; i++) {
        const CSzFileItem *f = ctx->db.db.Files + i;
        const size_t filename_offset = ctx->db.FileNameOffsets[i];
        const size_t filename_utf16le_len =  ctx->db.FileNameOffsets[i + 1] - filename_offset;
        const Byte *filename_utf16le = ctx->db.FileNamesInHeaderBufPtr + filename_offset * 2;
        /* 2 for UTF-18 + 3 for UTF-8. 1 UTF-16 entry point can create at most 3 UTF-8 bytes (averaging for surrogates). */
        size_t filename_utf8_len = filename_utf16le_len * 3;

        if (f->IsDir) {
            continue;
        }

        if (filename_utf8_len > filename_utf8_capacity) {
            SzFree(filename_utf8);
            if (filename_utf8_capacity == 0) filename_utf8_capacity = 128;
            while (filename_utf8_capacity < filename_utf8_len) {
                filename_utf8_capacity <<= 1;
            }
            if ((filename_utf8 = (Byte*)SzAlloc(filename_utf8_capacity)) == 0) {
                res = SZ_ERROR_MEM;
                break;
            }
        }

        if (!Utf16Le_To_Utf8(filename_utf8, &filename_utf8_len, filename_utf16le, filename_utf16le_len)) {
            res = SZ_ERROR_BAD_FILENAME;
            break;
        }

        if (!strcmp((const char*)filename_utf8, path)) {
            *fileIndex = i;
            break;
        }
    }

    SzFree(filename_utf8);

    return res;
}

int un7z_exists(un7z_t *ctx, const char *path) 
{
    UInt32 fileIndex;
    /* internal error */
    if (un7z_find(ctx, path, &fileIndex)) {
        return 0;
    }
    if (fileIndex == (UInt32)-1) {
        return 0;
    }
    return 1;
}

void* un7z_extract(un7z_t *ctx, const char *path, size_t *size) 
{
    UInt32 fileIndex;
    size_t offset = 0;
    size_t outSizeProcessed = 0;
    SRes err = SZ_OK;
    void *buffer;

    /* internal error */
    if (un7z_find(ctx, path, &fileIndex)) {
        return NULL;
    }
    /* not found */
    if (fileIndex == (UInt32)-1) {
        return NULL;
    }
    /* cache blockindex and allocation, for quick file lookup? */
    err = SzArEx_Extract(&ctx->db, &ctx->stream, fileIndex,
                &ctx->blockIndex, &ctx->outBuffer, &ctx->outBufferSize,
                &offset, &outSizeProcessed);
    /* extraction error */
    if (err) {
        return NULL;
    }

    buffer = malloc(outSizeProcessed);
    memcpy(buffer, ctx->outBuffer + offset, outSizeProcessed);
    *size = outSizeProcessed;

    return buffer;
}

/* <<<<<<<<<<<<<<<<<< VFS >>>>>>>>>>>>>>>>>> */


#define geterrstr(n) (n == 0 ? "" : strerror(n))

static char* file_extract(un7z_t *ctx, const char *path, size_t *size) 
{
	return (char*)un7z_extract(ctx, path, size);
}

#define file_extract_or_die(ctx, path, out, outsize) \
if (!(out = un7z_extract(ctx, path, outsize))) { \
	fprintf(stderr, "cannot extract file '%s': %s", path, geterrstr(errno)); \
	status = 1; \
	goto done; \
}

typedef enum {
	SCRIPT_INVALID,
	SCRIPT_REGULAR,
	SCRIPT_BINARY
} script_type_t;

int extract_script(un7z_t *ctx, const char *filename, char **output, size_t *output_size) 
{
	char path[256], *ext;
    *output = NULL;
	// try loading jsbin, if js script is not found
	if (!(*output = un7z_extract(ctx, filename, output_size))) {
		sprintf(path, "%s", filename);
		ext = strrchr(path, '.');
		if (!ext) {
			return SCRIPT_INVALID;
		}
		*ext = 0;
		strcat(path, ".jsbin");
		if ((*output = un7z_extract(ctx, path, output_size))) {
			return SCRIPT_BINARY;
		}
	}
	if (!*output)
		return SCRIPT_INVALID;
	return SCRIPT_REGULAR;
}

static int eval_buf(JSContext *ctx, const void *buf, int buf_len, const char *filename)
{
    JSValue val;
    int ret;

    val = JS_Eval(ctx, buf, buf_len, filename, JS_EVAL_TYPE_GLOBAL);

    if (JS_IsException(val)) {
        js_std_dump_error(ctx);
        ret = -1;
    } else {
        ret = 0;
    }
    JS_FreeValue(ctx, val);
    return ret;
}

static int eval_file(JSContext *ctx, const char *filename)
{
    uint8_t *buf;
    int ret;
    size_t buf_len;
    
    buf = js_std_load_file(ctx, &buf_len, filename);
    if (!buf) {
        perror(filename);
        return -1;
    }
    ret = eval_buf(ctx, buf, buf_len, filename);
    js_free(ctx, buf);
    return ret;
}

static int print_host_version(JSContext *ctx, JSValue manifest) 
{
	JSValue prop;
	const char *ver;
	if (!JS_IsObject(manifest))
		return 1;
	prop = JS_GetPropertyStr(ctx, manifest, "version");
	ver = JS_ToCString(ctx, prop);
	printf("v%s\n", ver);
	JS_FreeValue(ctx, prop);
	JS_FreeCString(ctx, ver);
	return 0;
}

static int print_host_about(JSContext *ctx, JSValue manifest) 
{
	const char *ver, *name, *desc;
	JSValue prop;
	if (!JS_IsObject(manifest))
		return 1;
	
	prop = JS_GetPropertyStr(ctx, manifest, "version");
	ver = JS_ToCString(ctx, prop);
	JS_FreeValue(ctx, prop);
	
	prop = JS_GetPropertyStr(ctx, manifest, "name");
	name = JS_ToCString(ctx, prop);
	JS_FreeValue(ctx, prop);

	prop = JS_GetPropertyStr(ctx, manifest, "description");
	desc = JS_ToCString(ctx, prop);
	JS_FreeValue(ctx, prop);

	printf("%s v%s\n", name, ver);
	printf("%s\n", desc);

	JS_FreeCString(ctx, ver);
	JS_FreeCString(ctx, name);
	JS_FreeCString(ctx, desc);

	return 0;
}

/* read internal file  */
JSValue js_std_readfile_internal(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    char *buf;
    const char *filename;
    JSValue ret;
    size_t buf_len;
    
    filename = JS_ToCString(ctx, argv[0]);
    if (!filename)
        return JS_EXCEPTION;

    buf = file_extract(&un7z_ctx, filename, &buf_len);
    if (!buf) {
    	JS_FreeCString(ctx, filename);
        return js_std_throw_error(ctx, geterrstr(errno), errno);
    }
    ret = JS_NewStringLen(ctx, buf, buf_len);
    free(buf);
    JS_FreeCString(ctx, filename);
    return ret;
}

/* read internal file  */
JSValue js_std_readfile_internal_raw(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    char *buf;
    const char *filename;
    JSValue ret;
    size_t buf_len;
    
    filename = JS_ToCString(ctx, argv[0]);
    if (!filename)
        return JS_EXCEPTION;

    buf = file_extract(&un7z_ctx, filename, &buf_len);
    if (!buf) {
    	JS_FreeCString(ctx, filename);
        return js_std_throw_error(ctx, geterrstr(errno), errno);
    }
    JS_FreeCString(ctx, filename);
    return js_std_blob_ptr(ctx, buf, buf_len, 0, 0);
}

/* check if internal file exists */
JSValue js_std_exists_internal(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    const char *filename;;
    
    filename = JS_ToCString(ctx, argv[0]);
    if (!filename)
        return JS_EXCEPTION;

    if (un7z_exists(&un7z_ctx, filename)) {
    	JS_FreeCString(ctx, filename);
		return JS_TRUE;
	}

    JS_FreeCString(ctx, filename);
    return JS_FALSE;
}

static void add_helpers(JSContext *ctx, int argc, char **argv)
{
    JSValue global_obj, console, args;
    int i;

    global_obj = JS_GetGlobalObject(ctx);

    args = JS_NewArray(ctx);
    for(i = 0; i < argc; i++) {
        JS_SetPropertyUint32(ctx, args, i, JS_NewString(ctx, argv[i]));
    }

    JS_SetPropertyStr(ctx, global_obj, "__scriptArgs", args);
    JS_SetPropertyStr(ctx, global_obj, "__print", JS_NewCFunction(ctx, js_std_print, "__print", 1));
    JS_SetPropertyStr(ctx, global_obj, "__printf", JS_NewCFunction(ctx, js_std_printf, "__printf", 1));
    JS_SetPropertyStr(ctx, global_obj, "__sprintf", JS_NewCFunction(ctx, js_std_sprintf, "__sprintf", 1));
    JS_SetPropertyStr(ctx, global_obj, "__compile", JS_NewCFunction(ctx, js_std_compile, "__compile", 2));
    JS_SetPropertyStr(ctx, global_obj, "__readfile", JS_NewCFunction(ctx, js_std_readfile, "__readfile", 1));
    JS_SetPropertyStr(ctx, global_obj, "__readfile_raw", JS_NewCFunction(ctx, js_std_readfile_raw, "__readfile_raw", 1));
    JS_SetPropertyStr(ctx, global_obj, "__writefile", JS_NewCFunction(ctx, js_std_writefile, "__writefile", 2));
    JS_SetPropertyStr(ctx, global_obj, "__readfile_internal", JS_NewCFunction(ctx, js_std_readfile_internal, "__readfile_internal", 1));
    JS_SetPropertyStr(ctx, global_obj, "__readfile_internal_raw", JS_NewCFunction(ctx, js_std_readfile_internal_raw, "__readfile_internal_raw", 1));
    JS_SetPropertyStr(ctx, global_obj, "__exists_internal", JS_NewCFunction(ctx, js_std_exists_internal, "__exists_internal", 1));
    JS_SetPropertyStr(ctx, global_obj, "__exists", JS_NewCFunction(ctx, js_std_exists, "__exists", 2));
    JS_SetPropertyStr(ctx, global_obj, "__exit", JS_NewCFunction(ctx, js_std_exit, "__exit", 1));
    JS_SetPropertyStr(ctx, global_obj, "__getenv", JS_NewCFunction(ctx, js_std_getenv, "__getenv", 1));
    JS_SetPropertyStr(ctx, global_obj, "__getcwd", JS_NewCFunction(ctx, js_std_getcwd, "__getcwd", 0));
    JS_SetPropertyStr(ctx, global_obj, "__realpath", JS_NewCFunction(ctx, js_std_realpath, "__realpath", 1));
    JS_SetPropertyStr(ctx, global_obj, "__mkdir", JS_NewCFunction(ctx, js_std_mkdir, "__mkdir", 2));
    JS_SetPropertyStr(ctx, global_obj, "__remove", JS_NewCFunction(ctx, js_std_remove, "__remove", 1));
    JS_SetPropertyStr(ctx, global_obj, "__readdir", JS_NewCFunction(ctx, js_std_readdir, "__readdir", 1));
    JS_SetPropertyStr(ctx, global_obj, "__stat", JS_NewCFunction(ctx, js_std_stat, "__stat", 1));
    JS_SetPropertyStr(ctx, global_obj, "__utimes", JS_NewCFunction(ctx, js_std_utimes, "__utimes", 3));
    JS_SetPropertyStr(ctx, global_obj, "__eval_bytecode", JS_NewCFunction(ctx, js_std_eval_bytecode, "__eval_bytecode", 2));
    JS_SetPropertyStr(ctx, global_obj, "__eval_module", JS_NewCFunction(ctx, js_std_eval_module, "__eval_module", 2));
    JS_SetPropertyStr(ctx, global_obj, "__eval_global", JS_NewCFunction(ctx, js_std_eval_global, "__eval_global", 2));
    JS_SetPropertyStr(ctx, global_obj, "__platform", JS_NewString(ctx, OS_PLATFORM));
    JS_SetPropertyStr(ctx, global_obj, "globalThis", JS_DupValue(ctx, global_obj));
    
    JS_FreeValue(ctx, global_obj);
}

int main(int argc, char **argv)
{
    SRes err;
    JSRuntime *rt;
    JSContext *ctx;
    JSValue obj;
    JSValue val;
    int i, res, status = 0;
    int run_external_script = 0;
    char *manifest_src = NULL, *bootstrap_src;
    size_t manifest_size, bootstrap_size;
    int (*host_action)(JSContext *ctx, JSValue manifest);
    const char *bootstrap_file = NULL;

    LOOKTOREAD_INIT(&un7z_ctx.stream);
    un7z_ctx.stream.data = pak_data;
    un7z_ctx.stream.data_len = pak_data_length;
    un7z_ctx.blockIndex = (UInt32)-1;
    un7z_ctx.outBufferSize = 0;
    un7z_ctx.outBuffer = NULL;

    err = SzArEx_Open(&un7z_ctx.db, &un7z_ctx.stream);

    if (err) {
		fprintf(stderr, "cannot allocate memory or read resources.\n");
		return 1;
	}
    
    if (!(rt = JS_NewRuntime())) {
        fprintf(stderr, PROG_NAME ": cannot allocate JS runtime\n");
        exit(2);
    }

    if (!(ctx = JS_NewContext(rt))) {
        fprintf(stderr, PROG_NAME ": cannot allocate JS context\n");
        exit(2);
    }

    file_extract_or_die(&un7z_ctx, MANIFEST_FILE, manifest_src, &manifest_size);
    manifest_src = realloc(manifest_src, manifest_size + 1);
    manifest_src[manifest_size] = 0;

    obj = JS_ParseJSON(ctx, manifest_src, manifest_size, "manifest.json");
	if (JS_IsException(obj)) {
		JSValue err = JS_GetException(ctx);
        const char *err_str = JS_ToCString(ctx, err);
        fprintf(stderr, "%s\n", err_str);
        JS_FreeValue(ctx, err);
        JS_FreeCString(ctx, err_str);
        JS_ResetUncatchableError(ctx);
        goto done;
	}

	for (i = 1; i < argc; ++i) {
		if (!strcmp(argv[i], "--host-version")) {
			host_action = print_host_version;
		} else if (!strcmp(argv[i], "--host-about")) {
			host_action = print_host_about;
		} else if (!strcmp(argv[i], "--host-script")) {
			run_external_script = 1;
			JS_FreeValue(ctx, obj);
            if (i + 1 < argc) {
                bootstrap_file = argv[++i];
            }
			break;
		} else {
			continue;
		}
		status = host_action(ctx, obj);
	    JS_FreeValue(ctx, obj);
		goto done;
	}

    if (run_external_script) {
        JSAtom atom;
        JSValue path, gobj, args;
    	if (!bootstrap_file) {
	    	goto done;
    	}
        for (i = 1; i < argc;) {
            if (!strcmp(argv[i++], "--")) {
                break;
            }
        }
        add_helpers(ctx, argc - i, argv + i);
        atom = JS_NewAtom(ctx, "unshift");
        path = JS_NewString(ctx, argv[0]); // prepend executable path
		gobj = JS_GetGlobalObject(ctx);
        args = JS_GetPropertyStr(ctx, gobj, "__scriptArgs");
        JS_Invoke(ctx, args, atom, 1, &path);
		JS_SetPropertyStr(ctx, gobj, "__filename", JS_NewString(ctx, bootstrap_file));
        JS_FreeValue(ctx, gobj);
        JS_FreeValue(ctx, args);
        JS_FreeAtom(ctx, atom);
		eval_file(ctx, bootstrap_file);
    	goto done;
    }

    add_helpers(ctx, argc, argv);

	val = JS_GetPropertyStr(ctx, obj, "index");
	if (JS_IsUndefined(val)) {
		status = 1;
		fprintf(stderr, "cannot find index script.\n");
		JS_FreeValue(ctx, obj);
		goto done;
	}

	bootstrap_file = JS_ToCString(ctx, val);

    JS_FreeValue(ctx, val);
    JS_FreeValue(ctx, obj);

    if (!bootstrap_file) {
		status = 1;
		fprintf(stderr, "cannot find index script.\n");
		JS_FreeCString(ctx, bootstrap_file);
    	goto done;
    }

    /* add bootstrap reference */
    obj = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, obj, "__filename", JS_NewString(ctx, bootstrap_file));
    JS_FreeValue(ctx, obj);

    res = extract_script(&un7z_ctx, bootstrap_file, &bootstrap_src, &bootstrap_size);
    if (res == SCRIPT_INVALID) {
    	status = 1;
    	fprintf(stderr, "cannot extract file '%s': %s", bootstrap_file, geterrstr(errno));
    	JS_FreeCString(ctx, bootstrap_file);
    	goto done;
    }

    if (res == SCRIPT_BINARY) {
    	obj = JS_ReadObject(ctx, (uint8_t*)bootstrap_src, bootstrap_size, JS_READ_OBJ_BYTECODE);
        if (JS_IsException(obj)) {
            status = 1;
            js_std_dump_error(ctx);
        } else {
	        val = JS_EvalFunction(ctx, obj);
	        if (JS_IsException(val)) {
	            status = 1;
	            js_std_dump_error(ctx);
	        } else {
		        JS_FreeValue(ctx, val);
	        }
        }
    } else {
	    bootstrap_src = realloc(bootstrap_src, bootstrap_size + 1);
	    bootstrap_src[bootstrap_size] = 0;
	    eval_buf(ctx, bootstrap_src, bootstrap_size, bootstrap_file);
    }

    free(bootstrap_src);
    JS_FreeCString(ctx, bootstrap_file);

done:
	free(manifest_src);
	SzArEx_Free(&un7z_ctx.db);
    SzFree(un7z_ctx.outBuffer);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);

    return status;	
}