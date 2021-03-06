//
//
// Support for the Windows Agent  modules
//
//

#include "stdafx.h"

#include "modules.h"

#include <fmt/format.h>

#include <filesystem>
#include <string>

#include "cfg.h"
#include "cma_core.h"
#include "common/cfg_info.h"
#include "common/wtools.h"
#include "logger.h"
#include "zip.h"

namespace cma::cfg::modules {

void Module::reset() noexcept {
    name_.clear();
    exts_.clear();
    exec_.clear();
    dir_.clear();
}

[[nodiscard]] bool Module::isModuleFile(const std::filesystem::path &file) const
    noexcept {
    try {
        return tools::IsEqual(name() + std::string(kExtension),
                              file.u8string());
    } catch (const std::exception &e) {
        XLOG::l("Failed something in isModuleFile '{}'", e.what());
        return false;
    }
}

[[nodiscard]] std::vector<Module> LoadFromConfig(const YAML::Node &yaml) {
    try {
        auto m = yaml[groups::kModules];

        // check enable
        auto enabled = GetVal(m, vars::kEnabled, true);
        if (!enabled) return {};

        // gather all modules in the table
        std::vector<Module> vec;
        auto module_array = GetArray<YAML::Node>(m, vars::kModulesTable);
        int index = 0;
        for (const auto &module_node : module_array) {
            Module m;
            ++index;
            if (!m.loadFrom(module_node) || !m.valid()) {
                XLOG::l.w("Skip module {}", index - 1);
                continue;
            }

            if (std::any_of(std::begin(vec), std::end(vec),
                            [m](const Module &vec_m) {
                                return vec_m.name() == m.name();
                            })) {
                XLOG::l.w("Skip module {} with duplicated name '{}'", index - 1,
                          m.name());
                continue;
            }

            vec.push_back(m);
        }
        XLOG::l.i("Processed [{}] modules", vec.size());
        return vec;

    } catch (const std::exception &e) {
        XLOG::l("Failed processing modules '{}'", e.what());
        return {};
    }

    return {};
}

[[nodiscard]] bool Module::loadFrom(const YAML::Node &node) {
    //
    //
    try {
        name_ = node[vars::kModulesName].as<std::string>();
        exec_ =
            wtools::ConvertToUTF16(node[vars::kModulesExec].as<std::string>());
        exts_ = GetArray<std::string>(node[vars::kModulesExts]);
        std::string dir;

        // dir is optional
        try {
            dir = node[vars::kModulesDir].as<std::string>();
        } catch (const std::exception &e) {
            XLOG::t("dir is missing or not valid, this is ok '{}'", e.what());
        }
        if (dir.empty()) {
            dir = defaults::kModulesDir;
        }

        dir_ = fmt::format(dir, name());

    } catch (const std::exception &e) {
        XLOG::l("failed loading module '{}'", e.what());
        reset();
        return false;
    }

    if (name().empty()) {
        XLOG::l("Name is absent or not valid");
        reset();
        return false;
    }
    return true;
}

void ModuleCommander::readConfig(YAML::Node &node) {
    modules_ = LoadFromConfig(node);
}

int ModuleCommander::findModuleFiles(const std::filesystem::path &root) {
    namespace fs = std::filesystem;
    files_.clear();
    auto src_root = root / dirs::kFileInstallDir;
    for (auto &m : modules_) {
        auto name = m.name();
        name += kExtension;
        std::error_code ec;
        if (fs::exists(src_root / name, ec)) {
            files_.emplace_back(src_root / name);
            XLOG::l.i("Module '{}' is added to the list",
                      files_.back().u8string());
        }
    }

    return static_cast<int>(files_.size());
}

bool CreateDir(const std::filesystem::path &mod) noexcept {
    namespace fs = std::filesystem;
    try {
        std::error_code ec;
        fs::create_directories(mod, ec);
        if (!fs::exists(mod, ec) || !fs::is_directory(mod, ec)) {
            XLOG::l("Failed to create folder '{}' error is '{}'",
                    mod.u8string(), ec.message());
            return false;
        }
    } catch (std::exception &e) {
        XLOG::l("Failed to create folders to install modules '{}'", e.what());
        return false;
    }

    return true;
}

PathVector ModuleCommander::ScanDir(const std::filesystem::path &dir) noexcept {
    namespace fs = std::filesystem;
    PathVector vec;
    for (auto &p : fs::directory_iterator(dir)) {
        std::error_code ec;
        auto const &path = p.path();
        if (fs::is_directory(path, ec)) continue;
        if (!fs::is_regular_file(path, ec)) continue;

        auto path_string = path.wstring();
        if (path_string.empty()) continue;

        vec.emplace_back(path);
    }

    return vec;
}

// check that name of the file is found among module names
bool ModuleCommander::isBelongsToModules(
    const std::filesystem::path &file) const noexcept {
    return std::any_of(
        std::begin(modules_), std::end(modules_), [file](const Module &m) {
            try {
                return tools::IsEqual(m.name() + std::string(kExtension),
                                      file.filename().u8string());
            } catch (const std::exception &e) {
                XLOG::l("Exception '{}' at ModuleCommander", e.what());
                return false;
            }
        });
}

// looks for the kTargetDir file in target_dir - this is symbolic link to folder
// for remove content
bool ModuleCommander::RemoveContentByTargetDir(
    const std::vector<std::wstring> &content,
    const std::filesystem::path &target_dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(target_dir, ec)) return false;

    if (!fs::exists(target_dir / kTargetDir, ec)) return false;

    auto dir =
        tools::ReadFileInString((target_dir / kTargetDir).wstring().c_str());

    if (dir.has_value() && fs::exists(*dir, ec) && fs::is_directory(*dir, ec)) {
        if (dir->size() < kResonableDirLengthMin) {
            XLOG::l("The dir '{}' is suspicious, skipping", *dir);
            return false;
        }
        fs::path d{*dir};
        for (auto line : content) {
            fs::remove_all(d / line, ec);
        }

        return true;
    }

    return false;
}

bool ModuleCommander::CreateFileForTargetDir(
    const std::filesystem::path &module_dir,
    const std::filesystem::path &target_dir) {
    namespace fs = std::filesystem;

    try {
        if (target_dir.u8string().size() < kResonableDirLengthMin) {
            XLOG::l("suspicious dir '{}' to create link",
                    target_dir.u8string());
            return false;
        }

        if (module_dir.u8string().size() < kResonableDirLengthMin) {
            XLOG::l("suspicious dir '{}' to create link",
                    module_dir.u8string());
            return false;
        }

        std::error_code ec;
        fs::create_directories(module_dir);

        std::ofstream ofs(module_dir / kTargetDir);

        if (!ofs) {
            XLOG::l("Can't open file {} error {}",
                    (module_dir / kTargetDir).u8string(), GetLastError());
            return false;
        }

        ofs << target_dir.u8string();
        return true;
    } catch (const std::exception &e) {
        XLOG::l(XLOG_FUNC + " Exception '{}' when creating '{}'", e.what(),
                (module_dir / kTargetDir).u8string());
        return false;
    }
}

bool ModuleCommander::UninstallModuleZip(
    const std::filesystem::path &file, const std::filesystem::path &mod_root) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(file, ec)) {
        XLOG::d.i("'{}' is absent, no need to uninstall", file.u8string());
        return false;
    }

    auto name = file.filename();
    name.replace_extension("");
    auto target_dir = mod_root / name;
    auto list = cma::tools::zip::List(file.wstring());
    RemoveContentByTargetDir(list, target_dir);

    fs::remove_all(target_dir, ec);
    fs::remove(file, ec);

    return true;
}

void ModuleCommander::CreateBackupFolder(const std::filesystem::path &user) {
    namespace fs = std::filesystem;
    auto mod_backup = ModuleCommander::GetModBackup(user);
    std::error_code ec;
    if (fs::exists(mod_backup, ec)) return;

    XLOG::d.i("creating backup folder for modules installing '{}'",
              mod_backup.u8string());

    fs::create_directories(ModuleCommander::GetModBackup(user), ec);
}

bool ModuleCommander::BackupModule(const std::filesystem::path &module_file,
                                   const std::filesystem::path &backup_file) {
    namespace fs = std::filesystem;
    std::error_code ec;
    auto ret = fs::copy_file(module_file, backup_file,
                             fs::copy_options::overwrite_existing, ec);
    if (ret) return true;

    XLOG::l.crit("Error [{}] '{}' installing new mod", ec.value(),
                 ec.message());
    return false;
}

bool ModuleCommander::PrepareCleanTargetDir(
    const std::filesystem::path &mod_dir) {
    namespace fs = std::filesystem;

    if (mod_dir.u8string().size() < kResonableDirLengthMin) {
        XLOG::l("target_dir '{}'is too short when installing new module '{}'",
                mod_dir.u8string());
        return false;
    }
    std::error_code ec;
    fs::remove_all(mod_dir, ec);
    fs::create_directories(mod_dir, ec);

    return true;
}

bool ModuleCommander::InstallModule(const Module &mod,
                                    const std::filesystem::path &root,
                                    const std::filesystem::path &user,
                                    InstallMode mode) {
    namespace fs = std::filesystem;

    auto backup_file = GetModBackup(user) / mod.name();
    backup_file += kExtension.data();
    auto module_file = root / dirs::kFileInstallDir / mod.name();
    module_file += kExtension.data();

    std::error_code ec;
    if (!fs::exists(module_file, ec) || fs::file_size(module_file) == 0) {
        UninstallModuleZip(backup_file, GetModInstall(user));
        XLOG::l.i(
            "Installation of the module '{}' is not required, module file '{}'is "
            "absent or too short. Backup will be uninstalled",
            mod.name(), module_file.u8string());
        return false;
    }

    if (cma::tools::AreFilesSame(backup_file, module_file) &&
        mode == InstallMode::normal) {
        XLOG::l.i(
            "Installation of the module '{}' is not required, module file '{}'is same",
            mod.name(), module_file.u8string());
        return false;
    }

    CreateBackupFolder(user);

    auto uninstalled = UninstallModuleZip(backup_file, GetModInstall(user));

    if (!BackupModule(module_file, backup_file)) return false;

    fs::path default_dir = GetModInstall(user) / mod.name();  // default
    fs::path actual_dir = user / mod.dir();
    if (!PrepareCleanTargetDir(default_dir)) return false;

    if (!fs::equivalent(default_dir, actual_dir)) {
        // establish symbolic link
        CreateFileForTargetDir(default_dir, actual_dir);
    }

    auto ret =
        cma::tools::zip::Extract(backup_file.wstring(), actual_dir.wstring());
    if (!ret) {
        XLOG::l(
            "Extraction failed: removing backup file '{}' and default dir '{}'",
            backup_file.u8string(), default_dir.u8string());
        fs::remove(backup_file, ec);
        fs::remove_all(default_dir);
    }

    return ret;
}

void ModuleCommander::installModules(const std::filesystem::path &root,
                                     const std::filesystem::path &user,
                                     InstallMode mode) const {
    namespace fs = std::filesystem;
    auto mod_root = GetModInstall(user);
    auto mod_backup = GetModBackup(user);
    if (!CreateDir(mod_root)) return;
    if (!CreateDir(mod_backup)) return;

    auto installed = ScanDir(mod_backup);

    for (auto &f : installed) {
        if (!isBelongsToModules(f)) {
            UninstallModuleZip(f, mod_root);
        }
    }

    for (auto &m : modules_) {
        InstallModule(m, root, user, mode);
    }
}

void ModuleCommander::InstallDefault(InstallMode mode) noexcept {
    try {
        auto root = GetRootDir();
        auto user = GetUserDir();
        auto yaml = GetLoadedConfig();
        XLOG::l.i("Reading module config {}",
                  mode == InstallMode::force ? "forced" : "normal");
        readConfig(yaml);
        XLOG::l.i("Finding modules");
        findModuleFiles(root);
        XLOG::l.i("Installing modules");
        installModules(root, user, mode);
    } catch (const std::exception &e) {
        XLOG::l("Exception installing modules '{}'", e.what());
    }
}
void ModuleCommander::LoadDefault() noexcept {
    try {
        auto yaml = GetLoadedConfig();
        XLOG::l.i("Loading module config");
        readConfig(yaml);
    } catch (const std::exception &e) {
        XLOG::l("Exception loading modules config '{}'", e.what());
    }
}
}  // namespace cma::cfg::modules
