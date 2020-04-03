/*! *****************************************************************************
Copyright (c) Microsoft Corporation. All rights reserved.
Licensed under the Apache License, Version 2.0 (the "License"); you may not use
this file except in compliance with the License. You may obtain a copy of the
License at http://www.apache.org/licenses/LICENSE-2.0

THIS CODE IS PROVIDED ON AN *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED
WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE,
MERCHANTABLITY OR NON-INFRINGEMENT.

See the Apache Version 2.0 License for specific language governing permissions
and limitations under the License.
***************************************************************************** */

var ts = require("./compiler.min.js");
var hostApi = rootModule.require('./hostapi.js')(ts);

var hostReport = {
	getCanonicalFileName: function (filename) { return filename },
	newLine: hostApi.newLine,
	getNewLine: function () { return hostApi.newLine },
	getCurrentDirectory: function() { return "" },
	write: function(msg) { hostApi.write(msg); },
	useCaseSensitiveFileNames: function() { return hostApi.useCaseSensitiveFileNames; }
}

var reportDiagnostic = ts.createDiagnosticReporter(hostReport);

module.exports = function transpile(commandLine, sys) {
  	if (commandLine.options.build) {
        reportDiagnostic(ts.createCompilerDiagnostic(ts.Diagnostics.Option_build_must_be_the_first_command_line_argument));
        return sys.exit(ts.ExitStatus.DiagnosticsPresent_OutputsSkipped);
    }
    // Configuration file name (if any)
    var configFileName;
    if (commandLine.options.locale) {
        ts.validateLocaleAndSetLanguage(commandLine.options.locale, sys, commandLine.errors);
    }
    // If there are any errors due to command line parsing and/or
    // setting up localization, report them and quit.
    if (commandLine.errors.length > 0) {
        commandLine.errors.forEach(reportDiagnostic);
        return sys.exit(ts.ExitStatus.DiagnosticsPresent_OutputsSkipped);
    }
    if (commandLine.options.init) {
        writeConfigFile(sys, reportDiagnostic, commandLine.options, commandLine.fileNames);
        return sys.exit(ts.ExitStatus.Success);
    }
    if (commandLine.options.version) {
        printVersion(sys);
        return sys.exit(ts.ExitStatus.Success);
    }
    if (commandLine.options.help || commandLine.options.all) {
        printVersion(sys);
        printHelp(sys, getOptionsForHelp(commandLine));
        return sys.exit(ts.ExitStatus.Success);
    }
    if (commandLine.options.watch && commandLine.options.listFilesOnly) {
        reportDiagnostic(ts.createCompilerDiagnostic(ts.Diagnostics.Options_0_and_1_cannot_be_combined, "watch", "listFilesOnly"));
        return sys.exit(ts.ExitStatus.DiagnosticsPresent_OutputsSkipped);
    }
    if (commandLine.options.project) {
        if (commandLine.fileNames.length !== 0) {
            reportDiagnostic(ts.createCompilerDiagnostic(ts.Diagnostics.Option_project_cannot_be_mixed_with_source_files_on_a_command_line));
            return sys.exit(ts.ExitStatus.DiagnosticsPresent_OutputsSkipped);
        }
        var fileOrDirectory = ts.normalizePath(commandLine.options.project);
        if (!fileOrDirectory /* current directory "." */ || sys.directoryExists(fileOrDirectory)) {
            configFileName = ts.combinePaths(fileOrDirectory, "tsconfig.json");
            if (!sys.fileExists(configFileName)) {
                reportDiagnostic(ts.createCompilerDiagnostic(ts.Diagnostics.Cannot_find_a_tsconfig_json_file_at_the_specified_directory_Colon_0, commandLine.options.project));
                return sys.exit(ts.ExitStatus.DiagnosticsPresent_OutputsSkipped);
            }
        }
        else {
            configFileName = fileOrDirectory;
            if (!sys.fileExists(configFileName)) {
                reportDiagnostic(ts.createCompilerDiagnostic(ts.Diagnostics.The_specified_path_does_not_exist_Colon_0, commandLine.options.project));
                return sys.exit(ts.ExitStatus.DiagnosticsPresent_OutputsSkipped);
            }
        }
    }
    else if (commandLine.fileNames.length === 0) {
        var searchPath = ts.normalizePath(sys.getCurrentDirectory());
        configFileName = ts.findConfigFile(searchPath, function (fileName) { return sys.fileExists(fileName); });
    }
    if (commandLine.fileNames.length === 0 && !configFileName) {
    	if (commandLine.options.showConfig) {
            reportDiagnostic(ts.createCompilerDiagnostic(ts.Diagnostics.Cannot_find_a_tsconfig_json_file_at_the_current_directory_Colon_0, ts.normalizePath(sys.getCurrentDirectory())));
        }
        else {
            printVersion(sys);
            printHelp(sys, getOptionsForHelp(commandLine));
        }
        return sys.exit(ts.ExitStatus.DiagnosticsPresent_OutputsSkipped);
    }
    var currentDirectory = sys.getCurrentDirectory();
    var commandLineOptions = ts.convertToOptionsWithAbsolutePaths(commandLine.options, function (fileName) { return ts.getNormalizedAbsolutePath(fileName, currentDirectory); });
    if (configFileName) {
        var configParseResult = ts.parseConfigFileWithSystem(configFileName, commandLineOptions, commandLine.watchOptions, sys, reportDiagnostic); // TODO: GH#18217
        if (commandLineOptions.showConfig) {
            if (configParseResult.errors.length !== 0) {
                reportDiagnostic = updateReportDiagnostic(sys, reportDiagnostic, configParseResult.options);
                configParseResult.errors.forEach(reportDiagnostic);
                return sys.exit(ts.ExitStatus.DiagnosticsPresent_OutputsSkipped);
            }
            // eslint-disable-next-line no-null/no-null
            sys.write(JSON.stringify(ts.convertToTSConfig(configParseResult, configFileName, sys), null, 4) + sys.newLine);
            return sys.exit(ts.ExitStatus.Success);
        }
        reportDiagnostic = updateReportDiagnostic(sys, reportDiagnostic, configParseResult.options);

        if (ts.isWatchSet(configParseResult.options)) {
            // if (reportWatchModeWithoutSysSupport(sys, reportDiagnostic))
            //     return;
            // return createWatchOfConfigFile(sys, cb, reportDiagnostic, configParseResult, commandLineOptions, commandLine.watchOptions);
			console.log('watch mode is not implementd');
        	return;
        }
        else if (ts.isIncrementalCompilation(configParseResult.options)) {
            performIncrementalCompilation(sys, reportDiagnostic, configParseResult);
        }
        else {
            performCompilation(sys, reportDiagnostic, configParseResult);
        }
    } else {
        if (commandLineOptions.showConfig) {
            // eslint-disable-next-line no-null/no-null
            sys.write(JSON.stringify(ts.convertToTSConfig(commandLine, ts.combinePaths(currentDirectory, "tsconfig.json"), sys), null, 4) + sys.newLine);
            return sys.exit(ts.ExitStatus.Success);
        }
        reportDiagnostic = updateReportDiagnostic(sys, reportDiagnostic, commandLineOptions);
        if (ts.isWatchSet(commandLineOptions)) {
            // if (reportWatchModeWithoutSysSupport(sys, reportDiagnostic))
            //     return;
            // return createWatchOfFilesAndCompilerOptions(sys, cb, reportDiagnostic, commandLine.fileNames, commandLineOptions, commandLine.watchOptions);
            console.log('watch mode is not implementd');
        	return;
        }
        else if (ts.isIncrementalCompilation(commandLineOptions)) {
            performIncrementalCompilation(sys, reportDiagnostic, ts.assign(ts.assign({}, commandLine), { options: commandLineOptions }));
        }
        else {
            performCompilation(sys, reportDiagnostic, ts.assign(ts.assign({}, commandLine), { options: commandLineOptions }));
        }
    }
}

function performCompilation(sys, reportDiagnostic, config) {
	var fileNames = config.fileNames, options = config.options, projectReferences = config.projectReferences;
    var host = ts.createCompilerHostWorker(options, /*setParentPos*/ undefined, sys);
    var currentDirectory = host.getCurrentDirectory();
    var getCanonicalFileName = ts.createGetCanonicalFileName(host.useCaseSensitiveFileNames());
    ts.changeCompilerHostLikeToUseCache(host, function (fileName) { return ts.toPath(fileName, currentDirectory, getCanonicalFileName); });
    // enableStatistics(sys, options);
    var programOptions = {
        rootNames: fileNames,
        options: options,
        projectReferences: projectReferences,
        host: host,
        configFileParsingDiagnostics: ts.getConfigFileParsingDiagnostics(config)
    };
    var program = ts.createProgram(programOptions);
    var emitResult = program.emit();
    var diagnostics = program.getSyntacticDiagnostics();
    diagnostics.forEach(reportDiagnostic); 
    // reportStatistics(sys, program);
    return emitResult.emitSkipped ? 1 : 0;
}

function performIncrementalCompilation(sys, reportDiagnostic, config) {
    var options = config.options, fileNames = config.fileNames, projectReferences = config.projectReferences;
    // enableStatistics(sys, options);
    var host = ts.createIncrementalCompilerHost(options, sys);
    var program = ts.createIncrementalProgram({
        host: host,
        rootNames: fileNames,
        options: options,
        configFileParsingDiagnostics: ts.getConfigFileParsingDiagnostics(config),
        projectReferences: projectReferences,
    });
    var emitResult = program.emit();
    var diagnostics = program.getSyntacticDiagnostics();
    diagnostics.forEach(reportDiagnostic); 
    // reportStatistics(sys, program);
    return emitResult.emitSkipped ? 1 : 0;
}

function createReportErrorSummary(sys, options) {
    return shouldBePretty(sys, options) ?
        function (errorCount) { return sys.write(ts.getErrorSummaryText(errorCount, sys.newLine)); } :
        undefined;
}

function defaultIsPretty(sys) {
    return !!sys.writeOutputIsTTY && sys.writeOutputIsTTY();
}
function shouldBePretty(sys, options) {
    if (!options || typeof options.pretty === "undefined") {
        return defaultIsPretty(sys);
    }
    return options.pretty;
}
function defaultIsPretty(sys) {
    return !!sys.writeOutputIsTTY && sys.writeOutputIsTTY();
}

function shouldBePretty(sys, options) {
    if (!options || typeof options.pretty === "undefined") {
        return defaultIsPretty(sys);
    }
    return options.pretty;
}

function updateReportDiagnostic(sys, existing, options) {
    return shouldBePretty(sys, options) ?
        ts.createDiagnosticReporter(sys, /*pretty*/ true) :
        existing;
}

function writeConfigFile(sys, reportDiagnostic, options, fileNames) {
    var currentDirectory = sys.getCurrentDirectory();
    var file = ts.normalizePath(ts.combinePaths(currentDirectory, "tsconfig.json"));
    if (sys.fileExists(file)) {
        reportDiagnostic(ts.createCompilerDiagnostic(ts.Diagnostics.A_tsconfig_json_file_is_already_defined_at_Colon_0, file));
    }
    else {
        sys.writeFile(file, ts.generateTSConfig(options, fileNames, sys.newLine));
        reportDiagnostic(ts.createCompilerDiagnostic(ts.Diagnostics.Successfully_created_a_tsconfig_json_file));
    }
    return;
}

function getOptionsForHelp(commandLine) {
    // Sort our options by their names, (e.g. "--noImplicitAny" comes before "--watch")
    return !!commandLine.options.all ?
        ts.sort(ts.optionDeclarations, function (a, b) { return ts.compareStringsCaseInsensitive(a.name, b.name); }) :
        ts.filter(ts.optionDeclarations.slice(), function (v) { return !!v.showInSimplifiedHelpView; });
}

function printVersion(sys) {
    sys.write(ts.getDiagnosticText(ts.Diagnostics.Version_0, ts.version) + sys.newLine);
}

function printHelp(sys, optionsList, syntaxPrefix) {
    if (syntaxPrefix === void 0) { syntaxPrefix = ""; }
    var output = [];
    // We want to align our "syntax" and "examples" commands to a certain margin.
    var syntaxLength = ts.getDiagnosticText(ts.Diagnostics.Syntax_Colon_0, "").length;
    var examplesLength = ts.getDiagnosticText(ts.Diagnostics.Examples_Colon_0, "").length;
    var marginLength = Math.max(syntaxLength, examplesLength);
    // Build up the syntactic skeleton.
    var syntax = makePadding(marginLength - syntaxLength);
    syntax += "tsc " + syntaxPrefix + "[" + ts.getDiagnosticText(ts.Diagnostics.options) + "] [" + ts.getDiagnosticText(ts.Diagnostics.file) + "...]";
    output.push(ts.getDiagnosticText(ts.Diagnostics.Syntax_Colon_0, syntax));
    output.push(sys.newLine + sys.newLine);
    // Build up the list of examples.
    var padding = makePadding(marginLength);
    output.push(ts.getDiagnosticText(ts.Diagnostics.Examples_Colon_0, makePadding(marginLength - examplesLength) + "tsc hello.ts") + sys.newLine);
    output.push(padding + "tsc --outFile file.js file.ts" + sys.newLine);
    output.push(padding + "tsc @args.txt" + sys.newLine);
    output.push(padding + "tsc --build tsconfig.json" + sys.newLine);
    output.push(sys.newLine);
    output.push(ts.getDiagnosticText(ts.Diagnostics.Options_Colon) + sys.newLine);
    // We want our descriptions to align at the same column in our output,
    // so we keep track of the longest option usage string.
    marginLength = 0;
    var usageColumn = []; // Things like "-d, --declaration" go in here.
    var descriptionColumn = [];
    var optionsDescriptionMap = ts.createMap(); // Map between option.description and list of option.type if it is a kind
    for (var _i = 0, optionsList_1 = optionsList; _i < optionsList_1.length; _i++) {
        var option = optionsList_1[_i];
        // If an option lacks a description,
        // it is not officially supported.
        if (!option.description) {
            continue;
        }
        var usageText_1 = " ";
        if (option.shortName) {
            usageText_1 += "-" + option.shortName;
            usageText_1 += getParamType(option);
            usageText_1 += ", ";
        }
        usageText_1 += "--" + option.name;
        usageText_1 += getParamType(option);
        usageColumn.push(usageText_1);
        var description = void 0;
        if (option.name === "lib") {
            description = ts.getDiagnosticText(option.description);
            var element = option.element;
            var typeMap = element.type;
            optionsDescriptionMap.set(description, ts.arrayFrom(typeMap.keys()).map(function (key) { return "'" + key + "'"; }));
        }
        else {
            description = ts.getDiagnosticText(option.description);
        }
        descriptionColumn.push(description);
        // Set the new margin for the description column if necessary.
        marginLength = Math.max(usageText_1.length, marginLength);
    }
    // Special case that can't fit in the loop.
    var usageText = " @<" + ts.getDiagnosticText(ts.Diagnostics.file) + ">";
    usageColumn.push(usageText);
    descriptionColumn.push(ts.getDiagnosticText(ts.Diagnostics.Insert_command_line_options_and_files_from_a_file));
    marginLength = Math.max(usageText.length, marginLength);
    // Print out each row, aligning all the descriptions on the same column.
    for (var i = 0; i < usageColumn.length; i++) {
        var usage = usageColumn[i];
        var description = descriptionColumn[i];
        var kindsList = optionsDescriptionMap.get(description);
        output.push(usage + makePadding(marginLength - usage.length + 2) + description + sys.newLine);
        if (kindsList) {
            output.push(makePadding(marginLength + 4));
            for (var _a = 0, kindsList_1 = kindsList; _a < kindsList_1.length; _a++) {
                var kind = kindsList_1[_a];
                output.push(kind + " ");
            }
            output.push(sys.newLine);
        }
    }
    for (var _b = 0, output_1 = output; _b < output_1.length; _b++) {
        var line = output_1[_b];
        sys.write(line);
    }
    return;
    function getParamType(option) {
        if (option.paramType !== undefined) {
            return " " + ts.getDiagnosticText(option.paramType);
        }
        return "";
    }
    function makePadding(paddingLength) {
        return Array(paddingLength + 1).join(" ");
    }
}
