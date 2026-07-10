#include "dpi_codegen/dpi_lib_link.h"

#include "util/source_loc.h"

#include <cstdlib>
#include <format>
#include <future>
#include <vector>

namespace {

std::string quoteShellArg(const std::filesystem::path& path) {
    std::string quoted = "'";
    for (char c : path.string()) {
        if (c == '\'') {
            quoted += "'\\''";
        } else {
            quoted += c;
        }
    }
    quoted += "'";
    return quoted;
}

void runCommand(const std::string& description, const std::vector<std::string>& args) {
    std::string command;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) command += ' ';
        command += args[i];
    }
    const int result = std::system(command.c_str());
    if (result != 0) {
        throw mate::CompilerError(std::format(
            "mate --dpi-lib: {} failed (exit status {}): {}", description, result, command));
    }
}

std::filesystem::path compileToObject(const mate::DpiLibLinkConfig& config,
                                      const std::filesystem::path& source) {
    std::filesystem::path object = source;
    object.replace_extension(".o");
    std::vector<std::string> args = {
        quoteShellArg(config.cxx),
        "-std=c++20",
        "-O1",
        "-fPIC",
    };
    for (const auto& flag : config.extra_cxx_flags) {
        args.push_back(quoteShellArg(flag));
    }
    args.push_back("-c");
    args.push_back(quoteShellArg(source));
    args.push_back("-o");
    args.push_back(quoteShellArg(object));
    for (const auto& dir : config.include_dirs) {
        args.push_back("-I" + quoteShellArg(dir));
    }
    runCommand(std::format("compiling '{}'", source.string()), args);
    return object;
}

// Extracts every object member of `archive` into its own subdirectory of
// `extract_root` (one subdirectory per archive, to avoid member-name
// collisions across different input libraries) and returns their paths.
std::vector<std::filesystem::path> extractArchiveObjects(
    const mate::DpiLibLinkConfig& config,
    const std::filesystem::path& archive,
    const std::filesystem::path& extract_root,
    size_t archive_index) {
    const std::filesystem::path dest = extract_root / std::to_string(archive_index);
    std::filesystem::create_directories(dest);
    // `cd` into the destination rather than relying on GNU ar's --output=
    // extension, since not every `ar` on a caller's system is guaranteed to
    // support it.
    runCommand(std::format("extracting '{}'", archive.string()),
              {"cd", quoteShellArg(dest), "&&", quoteShellArg(config.ar), "x",
               quoteShellArg(std::filesystem::absolute(archive))});

    std::vector<std::filesystem::path> objects;
    for (const auto& entry : std::filesystem::directory_iterator(dest)) {
        if (entry.path().extension() == ".o") objects.push_back(entry.path());
    }
    if (objects.empty()) {
        throw mate::CompilerError(std::format(
            "mate --dpi-lib: archive '{}' produced no object members", archive.string()));
    }
    return objects;
}

} // namespace

namespace mate {

void linkDpiLib(const DpiLibLinkConfig& config) {
    if (config.sources.empty()) {
        throw CompilerError("mate --dpi-lib: no source files to compile");
    }

    // Every source is an independent translation unit (that's the whole
    // point of splitting the generated combinational evaluator across
    // multiple files), so compile them concurrently rather than one process
    // at a time.
    std::vector<std::future<std::filesystem::path>> pending;
    pending.reserve(config.sources.size());
    for (const auto& source : config.sources) {
        pending.push_back(std::async(std::launch::async, compileToObject, std::cref(config),
                                     std::cref(source)));
    }
    std::vector<std::filesystem::path> objects;
    objects.reserve(pending.size());
    for (auto& future : pending) {
        objects.push_back(future.get());
    }

    const std::filesystem::path extract_root =
        config.out_lib.parent_path() / ".mate_dpi_lib_tmp";
    std::filesystem::remove_all(extract_root);
    for (size_t i = 0; i < config.link_libs.size(); ++i) {
        auto extracted = extractArchiveObjects(config, config.link_libs[i], extract_root, i);
        objects.insert(objects.end(), extracted.begin(), extracted.end());
    }

    if (std::filesystem::exists(config.out_lib)) {
        std::filesystem::remove(config.out_lib);
    }
    std::vector<std::string> ar_args = {quoteShellArg(config.ar), "rcs", quoteShellArg(config.out_lib)};
    for (const auto& object : objects) ar_args.push_back(quoteShellArg(object));
    runCommand(std::format("archiving '{}'", config.out_lib.string()), ar_args);

    std::filesystem::remove_all(extract_root);
}

} // namespace mate
