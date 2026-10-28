// Проверяет сохранение профилей и совместимости через публичные API коллекций.
// Все файлы принадлежат временным каталогам; GUI, облако и пользовательские настройки не используются.

#include <catch2/catch_all.hpp>

#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Model.hpp"
#include "test_utils.hpp"

#include <fstream>
#include <iterator>
#include <memory>

using namespace Slic3r;
namespace fs = boost::filesystem;

namespace {

// Загружает пользовательскую коллекцию из временного каталога, задавая также место последующего сохранения.
// Ошибки и подстановки формата не допускаются: тест записывает профиль текущей версии.
void load_user_presets(PresetCollection &presets, const fs::path &root, const std::string &subdir)
{
    PresetsConfigSubstitutions substitutions;
    presets.load_presets(root.string(), subdir, substitutions, ForwardCompatibilitySubstitutionRule::Disable);
    REQUIRE(substitutions.empty());
}

// Создаёт минимальный системный родитель в памяти; одинаковые значения позволяют повторить загрузку
// пользовательской копии в новом `PresetBundle`, не обращаясь к установленным профилям.
void add_system_filament(PresetBundle &bundle)
{
    DynamicPrintConfig config = bundle.filaments.default_preset().config;
    config.set_key_value("inherits", new ConfigOptionString(""));
    config.set_key_value("filament_settings_id", new ConfigOptionStrings{"System PLA"});
    config.set_key_value("nozzle_temperature", new ConfigOptionInts{210});
    config.set_key_value("compatible_printers", new ConfigOptionStrings{"Printer A"});
    config.set_key_value("compatible_printers_condition", new ConfigOptionString(""));
    Preset &preset = bundle.filaments.load_preset("", "System PLA", std::move(config), false, Semver(1, 0, 0));
    preset.is_system = true;
    preset.setting_id = "system-parent-id";
    preset.filament_id = "GFL99";
}

// Даёт одинаковую исходную конфигурацию для двух поддерживаемых типов; различение копий
// проверяется по явному списку совместимости, без зависимости от настроек установленного принтера.
PresetCollection &add_system_profile(PresetBundle &bundle, Preset::Type type)
{
    PresetCollection &presets = type == Preset::TYPE_PRINT ? bundle.prints : bundle.filaments;
    DynamicPrintConfig config = presets.default_preset().config;
    config.set_key_value("inherits", new ConfigOptionString(""));
    config.set_key_value("compatible_printers", new ConfigOptionStrings{"Printer A"});
    config.set_key_value("compatible_printers_condition", new ConfigOptionString(""));
    Preset &preset = presets.load_preset("", "System profile", std::move(config), false, Semver(1, 0, 0));
    preset.is_system = true;
    preset.setting_id = "system-id";
    presets.select_preset_by_name(preset.name, true);
    return presets;
}

// Читает собственный тестовый файл целиком, чтобы отличить сохранение профиля от случайной перезаписи проекта.
std::string saved_file_contents(const fs::path &path)
{
    std::ifstream stream(path.string(), std::ios::binary);
    REQUIRE(stream.is_open());
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

// Владеет объектами, которые импортёр архива возвращает через указатели; очищает их и при падении утверждения.
struct ImportedProjectData
{
    std::vector<Preset *> presets;
    PlateDataPtrs plates;
    ~ImportedProjectData()
    {
        for (Preset *preset : presets)
            delete preset;
        release_PlateData_list(plates);
    }
};

// Записывает и читает настоящий `.3mf` без GUI; импортированные указатели принадлежат `imported`.
// Геометрия нужна только для корректного архива, каталоги резервных копий изолированы и очищаются RAII.
void roundtrip_project(const std::string &archive, DynamicPrintConfig config, const std::vector<Preset *> &presets,
                       DynamicPrintConfig &loaded_config, ImportedProjectData &imported)
{
    ScopedTemporaryDir export_backup;
    Model model;
    model.add_object("Cube", "", make_cube(10, 10, 10));
    model.add_default_instances();
    model.set_backup_path(export_backup.string());
    StoreParams params;
    params.path = archive.c_str();
    params.model = &model;
    params.config = &config;
    params.project_presets = presets;
    params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence;
    REQUIRE(store_bbs_3mf(params));

    ScopedTemporaryDir import_backup;
    Model loaded_model;
    loaded_model.set_backup_path(import_backup.string());
    ConfigSubstitutionContext context{ForwardCompatibilitySubstitutionRule::Disable};
    bool is_bbl = false, is_orca = false;
    Semver version;
    REQUIRE(load_bbs_3mf(archive.c_str(), &loaded_config, &context, &loaded_model, &imported.plates,
                         &imported.presets, &is_bbl, &is_orca, &version, nullptr,
                         LoadStrategy::LoadModel | LoadStrategy::LoadConfig));
}

} // namespace

// Одинаковое имя не объединяет области: меняется только целевая запись и её выбранный ключ.
// Обратная перезапись библиотеки сохраняет её облачную идентичность и не меняет исходную проектную копию.
TEST_CASE("User and project profiles with the same name remain independent", "[PresetSaving][Regression]")
{
    const auto type = GENERATE(Preset::TYPE_FILAMENT, Preset::TYPE_PRINT);
    const bool detach = GENERATE(false, true);
    CAPTURE(type, detach);
    ScopedTemporaryDir temp;
    PresetBundle bundle;
    PresetCollection &presets = add_system_profile(bundle, type);
    load_user_presets(presets, temp.path(), Preset::get_type_string(type));
    presets.select_preset_by_name("System profile", true);
    presets.save_current_preset("Shared", true, false);
    presets.get_selected_preset().setting_id = "user-destination-id";
    presets.select_preset_by_name("Shared", true);
    const auto user_file = presets.get_selected_preset().file;
    const auto user_contents = saved_file_contents(user_file);
    presets.get_edited_preset().config.set_key_value("compatible_printers", new ConfigOptionStrings{"Printer B"});
    presets.save_current_preset("Shared", detach, true);

    REQUIRE(presets.get_selected_preset().is_project_embedded);
    CHECK(presets.get_selected_preset().setting_id.empty());
    CHECK(presets.get_selected_preset_name() == "_project/Shared");
    REQUIRE(presets.find_preset("Shared", false) != nullptr);
    CHECK(presets.find_preset("Shared")->is_user());
    CHECK(presets.find_preset("Shared")->config.option<ConfigOptionStrings>("compatible_printers")->values ==
          std::vector<std::string>{"Printer A"});
    CHECK(saved_file_contents(user_file) == user_contents);
    const auto project_config = presets.get_selected_preset().config;

    presets.get_edited_preset().config.set_key_value("compatible_printers", new ConfigOptionStrings{"Printer C"});
    presets.save_current_preset("Shared", detach, false);
    REQUIRE(presets.get_selected_preset().is_user());
    CHECK(presets.get_selected_preset_name() == "Shared");
    CHECK(presets.get_selected_preset().setting_id == "user-destination-id");
    CHECK(presets.get_selected_preset().inherits().empty());
    REQUIRE(presets.find_preset("_project/Shared", false) != nullptr);
    CHECK(presets.find_preset("_project/Shared")->config.diff(project_config).empty());
    presets.reset_project_embedded_presets();
    CHECK(presets.find_preset("_project/Shared", false) == nullptr);
    REQUIRE(presets.find_preset("Shared", false) != nullptr);
    CHECK(presets.find_preset("Shared")->config.option<ConfigOptionStrings>("compatible_printers")->values ==
          std::vector<std::string>{"Printer C"});
}

// Старый архив может содержать имя, уже присутствующее в библиотеке. Его запись должна загрузиться
// отдельно, а поиск неквалифицированного имени продолжает указывать на пользовательский профиль.
TEST_CASE("Legacy embedded profiles do not replace or hide same-named user profiles", "[PresetSaving][Regression]")
{
    const auto type = GENERATE(Preset::TYPE_FILAMENT, Preset::TYPE_PRINT);
    ScopedTemporaryDir temp;
    PresetBundle bundle;
    PresetCollection &presets = add_system_profile(bundle, type);
    load_user_presets(presets, temp.path(), Preset::get_type_string(type));
    presets.select_preset_by_name("System profile", true);
    presets.save_current_preset("Shared", true, false);
    Preset embedded(type, "Shared");
    embedded.is_project_embedded = true;
    embedded.config = presets.get_selected_preset().config;
    embedded.config.set_key_value("compatible_printers", new ConfigOptionStrings{"Printer B"});
    std::vector<Preset *> incoming{&embedded};
    PresetsConfigSubstitutions substitutions;
    presets.load_project_embedded_presets(incoming, Preset::get_type_string(type), substitutions,
                                          ForwardCompatibilitySubstitutionRule::Disable);
    REQUIRE(presets.find_preset("_project/Shared", false) != nullptr);
    CHECK(presets.find_preset("_project/Shared")->is_project_embedded);
    CHECK(presets.find_preset("_project/Shared")->config.option<ConfigOptionStrings>("compatible_printers")->values ==
          std::vector<std::string>{"Printer B"});
    REQUIRE(presets.find_preset("Shared", false) != nullptr);
    CHECK(presets.find_preset("Shared")->is_user());
    CHECK(presets.find_preset("Shared")->config.option<ConfigOptionStrings>("compatible_printers")->values ==
          std::vector<std::string>{"Printer A"});
}

// Вложенный профиль с путём исходного проекта копируется под прежним именем в пользовательскую область.
// Проверяется не только флаг в памяти, но и JSON, повторная загрузка и неизменность исходного файла.
TEST_CASE("A project filament creates a user copy without renaming", "[PresetSaving][Regression]")
{
    const bool detach = GENERATE(false, true);
    CAPTURE(detach);
    ScopedTemporaryDir temp;
    PresetBundle bundle;
    load_user_presets(bundle.filaments, temp.path(), PRESET_FILAMENT_NAME);
    add_system_filament(bundle);

    const fs::path project_file = temp.path() / "source.3mf";
    { std::ofstream stream(project_file.string()); stream << "original project"; }
    DynamicPrintConfig config = bundle.filaments.find_preset("System PLA")->config;
    config.set_key_value("inherits", new ConfigOptionString("System PLA"));
    config.set_key_value("filament_settings_id", new ConfigOptionStrings{"_project/Project PLA"});
    config.set_key_value("nozzle_temperature", new ConfigOptionInts{223});
    Preset &project = bundle.filaments.load_preset(project_file.string(), "_project/Project PLA", std::move(config));
    project.is_project_embedded = true;
    project.is_external = true;
    project.setting_id = "project-cloud-id";
    project.base_id = "system-parent-id";
    bundle.filaments.select_preset_by_name("_project/Project PLA", true);

    bundle.filaments.save_current_preset("Project PLA", detach, false);
    const Preset &saved = bundle.filaments.get_selected_preset();
    REQUIRE(saved.is_user());
    CHECK_FALSE(saved.is_external);
    CHECK_FALSE(saved.is_project_embedded);
    CHECK(saved.setting_id.empty());
    CHECK(saved.inherits() == (detach ? "" : "System PLA"));
    if (detach)
        CHECK(saved.base_id.empty());
    REQUIRE(saved.file != project_file.string());
    REQUIRE(fs::is_regular_file(saved.file));
    CHECK(saved_file_contents(project_file) == "original project");
    REQUIRE(bundle.filaments.find_preset("_project/Project PLA", false) != nullptr);
    CHECK(bundle.filaments.find_preset("_project/Project PLA")->is_project_embedded);

    // Повторное сохранение не должно возвращать тип проекта или самопроизвольно создавать родителя.
    bundle.filaments.save_current_preset("Project PLA", false, false);
    PresetBundle reloaded;
    add_system_filament(reloaded);
    load_user_presets(reloaded.filaments, temp.path(), PRESET_FILAMENT_NAME);
    const Preset *restored = reloaded.filaments.find_preset("Project PLA", false);
    REQUIRE(restored != nullptr);
    CHECK(restored->is_user());
    CHECK(restored->inherits() == (detach ? "" : "System PLA"));
    CHECK(restored->config.opt_int("nozzle_temperature", 0) == 223);
}

// Выбор проектного сохранения с отсоединением обязан экспортировать полную конфигурацию.
// Настоящий архив проходит штатные экспорт и импорт; новая коллекция не содержит системного родителя.
TEST_CASE("A detached project filament survives embedded preset export and reload", "[PresetSaving][Regression]")
{
    ScopedTemporaryDir temp;
    PresetBundle bundle;
    load_user_presets(bundle.filaments, temp.path(), PRESET_FILAMENT_NAME);
    add_system_filament(bundle);
    bundle.filaments.select_preset_by_name("System PLA", true);
    auto &config = bundle.filaments.get_edited_preset().config;
    config.set_key_value("nozzle_temperature", new ConfigOptionInts{231});
    config.set_key_value("compatible_printers", new ConfigOptionStrings{});
    bundle.filaments.save_current_preset("Independent project PLA", true, true);
    const Preset &saved = bundle.filaments.get_selected_preset();
    REQUIRE(saved.is_project_embedded);
    CHECK(saved.inherits().empty());
    CHECK_FALSE(fs::exists(saved.file));

    std::unique_ptr<Preset> embedded(bundle.filaments.get_preset_differed_for_save(bundle.filaments.get_selected_preset()));
    REQUIRE(embedded != nullptr);
    const std::string archive = (temp.path() / "project.3mf").string();
    DynamicPrintConfig project_config = DynamicPrintConfig::full_print_config();
    DynamicPrintConfig imported_config;
    ImportedProjectData imported;
    roundtrip_project(archive, project_config, {embedded.get()}, imported_config, imported);
    REQUIRE(imported.presets.size() == 1);
    CHECK(imported.presets.front()->filament_id == "GFL99");
    PresetBundle reloaded;
    PresetsConfigSubstitutions substitutions;
    reloaded.filaments.load_project_embedded_presets(imported.presets, PRESET_FILAMENT_NAME, substitutions,
                                                   ForwardCompatibilitySubstitutionRule::Disable);
    const Preset *restored = reloaded.filaments.find_preset(saved.name, false);
    REQUIRE(restored != nullptr);
    CHECK(restored->is_project_embedded);
    CHECK(restored->filament_id == "GFL99");
    CHECK(restored->inherits().empty());
    CHECK(restored->config.opt_int("nozzle_temperature", 0) == 231);
    CHECK(restored->config.option<ConfigOptionStrings>("compatible_printers")->values.empty());
}

// Отсутствующий непустой родитель не считается отсоединением: неполные отличия нельзя загрузить как полный профиль.
TEST_CASE("An embedded filament with a missing parent is not silently detached", "[PresetSaving]")
{
    PresetBundle bundle;
    Preset embedded(Preset::TYPE_FILAMENT, "Orphan");
    embedded.is_project_embedded = true;
    embedded.config.set_key_value("inherits", new ConfigOptionString("Missing parent"));
    embedded.config.set_key_value("nozzle_temperature", new ConfigOptionInts{231});
    std::vector<Preset *> presets{&embedded};
    PresetsConfigSubstitutions substitutions;
    bundle.filaments.load_project_embedded_presets(presets, PRESET_FILAMENT_NAME, substitutions,
                                                 ForwardCompatibilitySubstitutionRule::Disable);
    CHECK(bundle.filaments.find_preset("_project/Orphan", false) == nullptr);
}

// Первая копия системного профиля и последующая перезапись сохраняют список и условие буквально.
// Проверка повторной загрузки охватывает как полный JSON, так и отличия от системного родителя.
TEST_CASE("Filament and process saves preserve All and explicit printer compatibility", "[PresetSaving]")
{
    const auto type = GENERATE(Preset::TYPE_FILAMENT, Preset::TYPE_PRINT);
    const bool detach = GENERATE(false, true);
    const int compatibility = GENERATE(0, 1, 2);
    CAPTURE(type, detach, compatibility);
    const std::vector<std::string> printers = compatibility == 1 ?
        std::vector<std::string>{"Printer A", "Printer B"} : std::vector<std::string>{};
    const std::string condition = compatibility == 2 ? "printer_model == \"A\"" : "";
    ScopedTemporaryDir temp;
    PresetBundle bundle;
    auto &presets = add_system_profile(bundle, type);
    load_user_presets(presets, temp.path(), Preset::get_type_string(type));
    presets.select_preset_by_name("System profile", true);
    auto &config = presets.get_edited_preset().config;
    config.set_key_value("compatible_printers", new ConfigOptionStrings(printers));
    config.set_key_value("compatible_printers_condition", new ConfigOptionString(condition));
    presets.save_current_preset("User profile", detach, false);
    CHECK(presets.get_selected_preset().config.option<ConfigOptionStrings>("compatible_printers")->values == printers);
    presets.save_current_preset("User profile", detach, false);

    PresetBundle reloaded;
    auto &restored = add_system_profile(reloaded, type);
    load_user_presets(restored, temp.path(), Preset::get_type_string(type));
    const Preset *saved = restored.find_preset("User profile", false);
    REQUIRE(saved != nullptr);
    CHECK(saved->config.option<ConfigOptionStrings>("compatible_printers")->values == printers);
    CHECK(saved->compatible_printers_condition() == condition);
    CHECK(saved->inherits() == (detach ? "" : "System profile"));
    CHECK(presets.find_preset("System profile")->config.option<ConfigOptionStrings>("compatible_printers")->values ==
          std::vector<std::string>{"Printer A"});
}

// Проектная копия пользовательского профиля имеет отдельное имя и не изменяет запись библиотеки.
// После закрытия проекта исходный пользовательский профиль остаётся и в памяти, и на диске.
TEST_CASE("A project copy leaves the original user filament in the library", "[PresetSaving][Regression]")
{
    ScopedTemporaryDir temp;
    PresetBundle bundle;
    load_user_presets(bundle.filaments, temp.path(), PRESET_FILAMENT_NAME);
    add_system_filament(bundle);
    bundle.filaments.select_preset_by_name("System PLA", true);
    bundle.filaments.save_current_preset("User PLA", true, false);
    const fs::path user_file = bundle.filaments.get_selected_preset().file;
    const std::string user_contents = saved_file_contents(user_file);
    bundle.filaments.get_edited_preset().config.set_key_value("nozzle_temperature", new ConfigOptionInts{235});
    bundle.filaments.save_current_preset("Project copy", true, true);
    CHECK(bundle.filaments.get_selected_preset().is_project_embedded);
    CHECK(bundle.filaments.get_selected_preset().config.opt_int("nozzle_temperature", 0) == 235);
    CHECK_FALSE(fs::exists(bundle.filaments.get_selected_preset().file));
    bundle.filaments.reset_project_embedded_presets();
    CHECK(bundle.filaments.find_preset("_project/Project copy", false) == nullptr);
    const Preset *original = bundle.filaments.find_preset("User PLA", false);
    REQUIRE(original != nullptr);
    CHECK(original->is_user());
    CHECK(original->config.opt_int("nozzle_temperature", 0) == 210);
    CHECK(saved_file_contents(user_file) == user_contents);
}

// Пакетное сохранение использует те же параметры, что кнопка профиля, и сначала отменяет исключённые изменения.
// Все комбинации типа и отсоединения проверяются на существующем проектном имени.
TEST_CASE("Saving selected profile changes honors storage and detach choices", "[PresetSaving]")
{
    const auto type = GENERATE(Preset::TYPE_FILAMENT, Preset::TYPE_PRINT);
    const bool detach = GENERATE(false, true);
    const bool save_to_project = GENERATE(false, true);
    CAPTURE(type, detach, save_to_project);
    ScopedTemporaryDir temp;
    PresetBundle bundle;
    auto &presets = add_system_profile(bundle, type);
    load_user_presets(presets, temp.path(), Preset::get_type_string(type));
    presets.select_preset_by_name("System profile", true);
    presets.save_current_preset("Project profile", false, true);
    // В приложении сохраняемый материал занимает слот; проверяем также обновление его выбора в пакете.
    bundle.set_num_filaments(1);
    if (type == Preset::TYPE_FILAMENT)
        bundle.set_filament_preset(0, presets.get_selected_preset_name());
    auto &config = presets.get_edited_preset().config;
    config.set_key_value("compatible_printers", new ConfigOptionStrings{"Printer B"});
    config.set_key_value("compatible_printers_condition", new ConfigOptionString("printer_model == \"B\""));
    bundle.save_changes_for_preset("Project profile", type, {"compatible_printers"}, save_to_project, detach);
    const Preset &saved = presets.get_selected_preset();
    CHECK(saved.is_project_embedded == save_to_project);
    CHECK(saved.inherits() == (detach ? "" : "System profile"));
    CHECK(saved.config.option<ConfigOptionStrings>("compatible_printers")->values == std::vector<std::string>{"Printer A"});
    CHECK(saved.compatible_printers_condition() == "printer_model == \"B\"");
    CHECK(fs::is_regular_file(saved.file) == !save_to_project);
    if (type == Preset::TYPE_FILAMENT) {
        REQUIRE(bundle.filament_presets.size() == 1);
        CHECK(bundle.filament_presets.front() == saved.name);
    }
}

// Снятый `detach` при новом имени независимого пользовательского профиля создаёт наследование от него.
// При прежнем имени повторное сохранение не должно превращаться в наследование от самого себя.
TEST_CASE("A new filament copy can inherit from a standalone user preset", "[PresetSaving]")
{
    ScopedTemporaryDir temp;
    PresetBundle bundle;
    load_user_presets(bundle.filaments, temp.path(), PRESET_FILAMENT_NAME);
    add_system_filament(bundle);
    bundle.filaments.select_preset_by_name("System PLA", true);
    bundle.filaments.save_current_preset("User parent", true, false);
    bundle.filaments.save_current_preset("User child", false, false);
    CHECK(bundle.filaments.get_selected_preset().inherits() == "User parent");
    bundle.filaments.save_current_preset("User child", false, false);
    CHECK(bundle.filaments.get_selected_preset().inherits() == "User parent");

    PresetBundle reloaded;
    load_user_presets(reloaded.filaments, temp.path(), PRESET_FILAMENT_NAME);
    REQUIRE(reloaded.filaments.find_preset("User child", false) != nullptr);
    CHECK(reloaded.filaments.find_preset("User child")->inherits() == "User parent");
}

namespace {

// Создаёт три системных FFF-принтера и пользовательского потомка первого для проверок пересчёта совместимости.
// Настройки берутся из штатной конфигурации, а различающие их имена и модели задаются явно.
void add_test_printers(PresetBundle &bundle)
{
    for (const std::string model : {"A", "B", "C"}) {
        DynamicPrintConfig config = bundle.printers.default_preset().config;
        config.set_key_value("printer_model", new ConfigOptionString(model));
        config.set_key_value("nozzle_diameter", new ConfigOptionFloats{0.4});
        config.set_key_value("inherits", new ConfigOptionString(""));
        Preset &printer = bundle.printers.load_preset("", "Printer " + model, std::move(config), false);
        printer.is_system = true;
    }
    DynamicPrintConfig config = bundle.printers.find_preset("Printer A")->config;
    config.set_key_value("inherits", new ConfigOptionString("Printer A"));
    bundle.printers.load_preset("", "Custom A", std::move(config), false);
}

// Сохраняет самостоятельный профиль печати с заданной совместимостью и его наследуемую копию.
// Оба используют реальный путь записи JSON; родитель должен загрузиться раньше дочерних отличий.
void save_process_family(PresetBundle &bundle, const std::vector<std::string> &printers, const std::string &condition)
{
    DynamicPrintConfig config = bundle.prints.default_preset().config;
    config.set_key_value("inherits", new ConfigOptionString(""));
    config.set_key_value("compatible_printers", new ConfigOptionStrings(printers));
    config.set_key_value("compatible_printers_condition", new ConfigOptionString(condition));
    Preset &source = bundle.prints.load_preset("", "System process", std::move(config), false, Semver(1, 0, 0));
    source.is_system = true;
    bundle.prints.select_preset_by_name("System process", true);
    bundle.prints.save_current_preset("Shared process", true, false);
    bundle.prints.save_current_preset("Inherited process", false, false);
}

} // namespace

// Два выбранных принтера используют один профиль и его наследника; третий несовместим.
// Проверяется публичный пересчёт пакета после повторной загрузки, включая пользовательского потомка принтера.
TEST_CASE("One saved process remains compatible with multiple printers after switching", "[PresetSaving]")
{
    ScopedTemporaryDir temp;
    PresetBundle bundle;
    load_user_presets(bundle.prints, temp.path(), PRESET_PRINT_NAME);
    save_process_family(bundle, {"Printer A", "Printer B"}, "printer_model == \"C\"");

    PresetBundle reloaded;
    load_user_presets(reloaded.prints, temp.path(), PRESET_PRINT_NAME);
    add_test_printers(reloaded);
    const size_t profile_count = reloaded.prints.size();
    for (const std::string name : {"Printer A", "Printer B", "Printer C", "Custom A", "Printer A"}) {
        CAPTURE(name);
        reloaded.printers.select_preset_by_name(name, true);
        reloaded.prints.select_preset_by_name("Inherited process", true);
        reloaded.update_compatible(PresetSelectCompatibleType::Never);
        for (const std::string process : {"Shared process", "Inherited process"}) {
            const Preset *preset = reloaded.prints.find_preset(process, false);
            REQUIRE(preset != nullptr);
            CHECK(preset->is_compatible == (name != "Printer C"));
        }
        CHECK(reloaded.prints.size() == profile_count);
    }
    CHECK(reloaded.prints.find_preset("Inherited process")->inherits() == "Shared process");
}

// Пустой список переопределяет ограниченный список родителя. После записи отличий и загрузки `All`
// работает без условия для всех принтеров, а с условием — только для удовлетворяющих выражению.
TEST_CASE("An inherited process can save All and a compatibility condition", "[PresetSaving]")
{
    const bool use_condition = GENERATE(false, true);
    const std::string condition = use_condition ? "printer_model == \"A\"" : "";
    ScopedTemporaryDir temp;
    PresetBundle bundle;
    load_user_presets(bundle.prints, temp.path(), PRESET_PRINT_NAME);
    save_process_family(bundle, {"Printer B"}, "");
    auto &config = bundle.prints.get_edited_preset().config;
    config.set_key_value("compatible_printers", new ConfigOptionStrings{});
    config.set_key_value("compatible_printers_condition", new ConfigOptionString(condition));
    bundle.prints.save_current_preset("Inherited process", false, false);
    bundle.prints.save_current_preset("Grandchild process", false, false);

    PresetBundle reloaded;
    load_user_presets(reloaded.prints, temp.path(), PRESET_PRINT_NAME);
    add_test_printers(reloaded);
    for (const std::string name : {"Printer A", "Printer B", "Printer C"}) {
        CAPTURE(use_condition, name);
        const bool compatible = !use_condition || name == "Printer A";
        reloaded.printers.select_preset_by_name(name, true);
        reloaded.update_compatible(PresetSelectCompatibleType::Never);
        for (const std::string process : {"Inherited process", "Grandchild process"}) {
            const Preset *preset = reloaded.prints.find_preset(process, false);
            REQUIRE(preset != nullptr);
            CHECK(preset->config.option<ConfigOptionStrings>("compatible_printers")->values.empty());
            CHECK(preset->compatible_printers_condition() == condition);
            CHECK(preset->is_compatible == compatible);
        }
        CHECK(reloaded.prints.find_preset("Shared process")->is_compatible == (name == "Printer B"));
    }
}

// Выбор типа и `detach` не разрешает перезапись системного профиля или профиля пакета.
// После отказа коллекция остаётся пригодной для обычного сохранения под новым именем.
TEST_CASE("Saving a filament never overwrites a protected preset", "[PresetSaving]")
{
    const bool from_bundle = GENERATE(false, true);
    const bool detach = GENERATE(false, true);
    const bool save_to_project = GENERATE(false, true);
    CAPTURE(from_bundle, detach, save_to_project);
    ScopedTemporaryDir temp;
    PresetBundle bundle;
    load_user_presets(bundle.filaments, temp.path(), PRESET_FILAMENT_NAME);
    add_system_filament(bundle);
    if (from_bundle) {
        Preset *preset = bundle.filaments.find_preset("System PLA");
        preset->is_system = false;
        preset->bundle_id = "test-bundle";
    }
    bundle.filaments.select_preset_by_name("System PLA", true);
    bundle.filaments.get_edited_preset().config.set_key_value("nozzle_temperature", new ConfigOptionInts{225});
    bundle.filaments.save_current_preset("System PLA", detach, save_to_project);
    const Preset &original = bundle.filaments.get_selected_preset();
    CHECK_FALSE(original.can_overwrite());
    CHECK(original.config.opt_int("nozzle_temperature", 0) == 210);
    CHECK_FALSE(original.is_project_embedded);

    bundle.filaments.save_current_preset("User copy", true, false);
    CHECK(bundle.filaments.get_selected_preset().is_user());
    CHECK(bundle.filaments.get_selected_preset().config.opt_int("nozzle_temperature", 0) == 225);
}

// Проверка подтверждения сравнивает полные ключи. Пока сохранение не вызвано, отмена не меняет записи и файлы.
TEST_CASE("Overwrite decisions distinguish storage scopes and preserve cancelled destinations", "[PresetSaving]")
{
    const auto type = GENERATE(Preset::TYPE_FILAMENT, Preset::TYPE_PRINT);
    ScopedTemporaryDir temp;
    PresetBundle bundle;
    auto &presets = add_system_profile(bundle, type);
    load_user_presets(presets, temp.path(), Preset::get_type_string(type));
    presets.select_preset_by_name("System profile", true);
    presets.save_current_preset("Shared", true, false);
    CHECK_FALSE(presets.save_requires_confirmation("Shared", false));
    CHECK_FALSE(presets.save_requires_confirmation("Shared", true));
    presets.save_current_preset("Shared", true, true);
    CHECK_FALSE(presets.save_requires_confirmation("Shared", true));
    CHECK(presets.save_requires_confirmation("Shared", false));
    const auto user_file = presets.find_preset("Shared")->file;
    const auto original = saved_file_contents(user_file);
    CHECK(presets.saved_preset_name("_project/Shared", false) == "Shared");
    CHECK(presets.saved_preset_name("Shared", true) == "_project/Shared");
    CHECK(saved_file_contents(user_file) == original);
    presets.select_preset_by_name("Shared", true);
    CHECK(presets.save_requires_confirmation("Shared", true));
    CHECK_FALSE(presets.save_requires_confirmation("Shared", false));
}

// Исходный ключ относится к подтверждённому диалогу. Его несовпадение отклоняется до отката параметров и записи.
TEST_CASE("Saving selected changes rejects a different source profile", "[PresetSaving]")
{
    const auto type = GENERATE(Preset::TYPE_FILAMENT, Preset::TYPE_PRINT);
    ScopedTemporaryDir temp;
    PresetBundle bundle;
    auto &presets = add_system_profile(bundle, type);
    load_user_presets(presets, temp.path(), Preset::get_type_string(type));
    presets.select_preset_by_name("System profile", true);
    presets.save_current_preset("Shared", true, false);
    const auto user_file = presets.get_selected_preset().file;
    const auto original = saved_file_contents(user_file);
    REQUIRE_THROWS(bundle.save_changes_for_preset("Shared", type, {}, false, true, "_project/Shared"));
    CHECK(presets.get_selected_preset_name() == "Shared");
    CHECK(saved_file_contents(user_file) == original);
}

// Вложенный ребёнок может предшествовать родителю в архиве. Пользовательская копия не сохраняет
// временного проектного родителя; цикл во входных вложенных записях не подменяется библиотечными профилями.
TEST_CASE("Project inheritance resolves independently of archive order and never escapes into user copies", "[PresetSaving]")
{
    const auto type = GENERATE(Preset::TYPE_FILAMENT, Preset::TYPE_PRINT);
    ScopedTemporaryDir temp;
    PresetBundle bundle;
    auto &presets = add_system_profile(bundle, type);
    load_user_presets(presets, temp.path(), Preset::get_type_string(type));
    Preset parent(type, "Parent");
    parent.is_project_embedded = true;
    parent.config = presets.find_preset("System profile")->config;
    parent.inherits() = "System profile";
    Preset child(type, "Child");
    child.is_project_embedded = true;
    child.config.set_key_value("inherits", new ConfigOptionString("Parent"));
    child.config.set_key_value("compatible_printers", new ConfigOptionStrings{"Printer B"});
    std::vector<Preset *> incoming{&child, &parent};
    PresetsConfigSubstitutions substitutions;
    presets.load_project_embedded_presets(incoming, Preset::get_type_string(type), substitutions,
                                          ForwardCompatibilitySubstitutionRule::Disable);
    REQUIRE(presets.find_preset("_project/Child", false) != nullptr);
    CHECK(presets.find_preset("_project/Child")->inherits() == "_project/Parent");
    presets.select_preset_by_name("_project/Child", true);
    const auto source_config = presets.get_selected_preset().config;
    presets.save_current_preset("Child", false, false);
    CHECK(presets.get_selected_preset().inherits() == "System profile");
    CHECK(presets.find_preset("_project/Child")->config.diff(source_config).empty());
    presets.reset_project_embedded_presets();
    REQUIRE(presets.find_preset("Child", false) != nullptr);
    CHECK(presets.find_preset("Child")->config.option<ConfigOptionStrings>("compatible_printers")->values ==
          std::vector<std::string>{"Printer B"});

    parent.name = "_project/Cycle A";
    parent.inherits() = "_project/Cycle B";
    child.name = "_project/Cycle B";
    child.inherits() = "_project/Cycle A";
    presets.load_project_embedded_presets(incoming, Preset::get_type_string(type), substitutions,
                                          ForwardCompatibilitySubstitutionRule::Disable);
    CHECK(presets.find_preset("_project/Cycle A", false) == nullptr);
    CHECK(presets.find_preset("_project/Cycle B", false) == nullptr);
}

// Старые неквалифицированные ссылки мигрируют вместе с вложенными профилями, включая группу родителей.
// Новая каноническая ссылка остаётся неизменной, обычная ссылка на запись библиотеки не переписывается без основания.
TEST_CASE("Legacy project selections migrate with embedded profile names", "[PresetSaving][Regression]")
{
    PresetBundle bundle;
    Preset print(Preset::TYPE_PRINT, "Shared process"), filament(Preset::TYPE_FILAMENT, "Shared filament");
    print.is_project_embedded = filament.is_project_embedded = true;
    print.config = bundle.prints.default_preset().config;
    filament.config = bundle.filaments.default_preset().config;
    print.inherits().clear();
    filament.inherits().clear();
    DynamicPrintConfig config;
    config.set_key_value("print_settings_id", new ConfigOptionString(print.name));
    config.set_key_value("filament_settings_id", new ConfigOptionStrings{filament.name, "Library filament", "_project/Shared filament"});
    config.set_key_value("inherits_group", new ConfigOptionStrings{print.name, filament.name, "Library filament", "System printer"});
    REQUIRE_NOTHROW(bundle.load_project_embedded_presets({&print, &filament}, ForwardCompatibilitySubstitutionRule::Disable, &config));
    CHECK(config.opt_string("print_settings_id") == "_project/Shared process");
    CHECK(config.option<ConfigOptionStrings>("filament_settings_id")->values ==
          std::vector<std::string>{"_project/Shared filament", "Library filament", "_project/Shared filament"});
    CHECK(config.option<ConfigOptionStrings>("inherits_group")->values ==
          std::vector<std::string>{"Shared process", "Shared filament", "Library filament", "System printer"});
}

// Настоящий архив хранит проектную запись отдельно от одноимённой библиотечной и возвращает точные ссылки.
// Для пластика одновременно заняты два разных слота, для печати проверяются оба варианта выбранной копии.
TEST_CASE("A 3mf roundtrip preserves same-named profiles and their selected storage scopes", "[PresetSaving][Regression]")
{
    const auto type = GENERATE(Preset::TYPE_FILAMENT, Preset::TYPE_PRINT);
    const bool select_project = GENERATE(false, true);
    const bool detach = GENERATE(false, true);
    const bool equal_values = GENERATE(false, true);
    CAPTURE(type, select_project, detach, equal_values);
    const std::string project_note = equal_values ? "library" : "project";
    ScopedTemporaryDir temp;
    PresetBundle source;
    auto &presets = add_system_profile(source, type);
    load_user_presets(presets, temp.path(), Preset::get_type_string(type));
    presets.select_preset_by_name("System profile", true);
    presets.get_edited_preset().config.set_key_value("compatible_printers", new ConfigOptionStrings{});
    const char *note = type == Preset::TYPE_PRINT ? "notes" : "filament_notes";
    if (type == Preset::TYPE_PRINT)
        presets.get_edited_preset().config.set_key_value(note, new ConfigOptionString("library"));
    else
        presets.get_edited_preset().config.set_key_value(note, new ConfigOptionStrings{"library"});
    presets.save_current_preset("Shared", true, false);
    if (type == Preset::TYPE_PRINT)
        presets.get_edited_preset().config.set_key_value(note, new ConfigOptionString(project_note));
    else
        presets.get_edited_preset().config.set_key_value(note, new ConfigOptionStrings{project_note});
    presets.save_current_preset("Shared", detach, true);
    presets.select_preset_by_name(select_project ? "_project/Shared" : "Shared", true);
    source.set_num_filaments(type == Preset::TYPE_FILAMENT ? 2 : 1);
    if (type == Preset::TYPE_FILAMENT) {
        source.set_filament_preset(0, "Shared");
        source.set_filament_preset(1, "_project/Shared");
    }
    ImportedProjectData exported, imported;
    exported.presets = source.get_current_project_embedded_presets();
    REQUIRE(exported.presets.size() == 1);
    DynamicPrintConfig loaded_config;
    const auto archive = (temp.path() / "scopes.3mf").string();
    // Как `Plater::export_3mf`, сохраняем все варианты материалов, а не конфигурацию для нарезки.
    roundtrip_project(archive, source.full_config_secure(), exported.presets, loaded_config, imported);

    PresetBundle restored;
    auto &restored_presets = type == Preset::TYPE_PRINT ? restored.prints : restored.filaments;
    load_user_presets(restored_presets, temp.path(), Preset::get_type_string(type));
    restored.load_project_embedded_presets(imported.presets, ForwardCompatibilitySubstitutionRule::Disable, &loaded_config);
    REQUIRE(restored_presets.find_preset("Shared", false) != nullptr);
    REQUIRE(restored_presets.find_preset("_project/Shared", false) != nullptr);
    CHECK(restored_presets.find_preset("Shared")->config.option(note)->serialize() == "library");
    CHECK(restored_presets.find_preset("_project/Shared")->config.option(note)->serialize() == project_note);
    restored.load_config_model(archive, std::move(loaded_config));
    if (type == Preset::TYPE_FILAMENT) {
        CHECK(restored.filament_presets == std::vector<std::string>{"Shared", "_project/Shared"});
    } else {
        CHECK(restored.prints.get_selected_preset_name() == (select_project ? "_project/Shared" : "Shared"));
    }
}

// Проектный ребёнок не имеет JSON: сохранение библиотечного родителя обновляет унаследованные значения
// в памяти, сохраняя явные отличия ребёнка и его проектную идентичность.
TEST_CASE("Saving a user parent updates its project child without reading a user file", "[PresetSaving][Regression]")
{
    const auto type = GENERATE(Preset::TYPE_FILAMENT, Preset::TYPE_PRINT);
    ScopedTemporaryDir temp;
    PresetBundle bundle;
    auto &presets = add_system_profile(bundle, type);
    load_user_presets(presets, temp.path(), Preset::get_type_string(type));
    presets.select_preset_by_name("System profile", true);
    presets.save_current_preset("Parent", true, false);
    presets.get_edited_preset().config.set_key_value("compatible_printers", new ConfigOptionStrings{"Printer B"});
    presets.save_current_preset("Child", false, true);
    presets.select_preset_by_name("Parent", true);
    presets.get_edited_preset().config.set_key_value("compatible_printers_condition", new ConfigOptionString("printer_model == \"A\""));
    REQUIRE_NOTHROW(presets.save_current_preset("Parent", true, false));
    const Preset *child = presets.find_preset("_project/Child", false, true);
    REQUIRE(child != nullptr);
    CHECK(child->is_project_embedded);
    CHECK(child->inherits() == "Parent");
    CHECK(child->compatible_printers_condition() == "printer_model == \"A\"");
    CHECK(child->config.option<ConfigOptionStrings>("compatible_printers")->values == std::vector<std::string>{"Printer B"});
}

// Отсутствующий проектный родитель не заменяется одноимённой записью библиотеки.
// Ошибка возникает до перезаписи назначения в памяти и на диске.
TEST_CASE("A missing scoped parent rejects saving before changing the destination", "[PresetSaving][Regression]")
{
    const auto type = GENERATE(Preset::TYPE_FILAMENT, Preset::TYPE_PRINT);
    ScopedTemporaryDir temp;
    PresetBundle bundle;
    auto &presets = add_system_profile(bundle, type);
    load_user_presets(presets, temp.path(), Preset::get_type_string(type));
    presets.select_preset_by_name("System profile", true);
    presets.save_current_preset("Missing", true, false);
    presets.save_current_preset("Destination", true, false);
    const Preset original = presets.get_selected_preset();
    const auto contents = saved_file_contents(original.file);
    presets.get_edited_preset().inherits() = "_project/Missing";
    REQUIRE_THROWS(presets.save_current_preset("Destination", false, false));
    CHECK(presets.get_selected_preset().config.diff(original.config).empty());
    CHECK(saved_file_contents(original.file) == contents);
}

// При пакетном сохранении меняются ссылки на точный источник, но не на одноимённый профиль другой области.
TEST_CASE("Saving selected filament changes remaps only slots with the exact source key", "[PresetSaving][Regression]")
{
    ScopedTemporaryDir temp;
    PresetBundle bundle;
    auto &presets = add_system_profile(bundle, Preset::TYPE_FILAMENT);
    load_user_presets(presets, temp.path(), PRESET_FILAMENT_NAME);
    presets.select_preset_by_name("System profile", true);
    presets.save_current_preset("Shared", true, false);
    presets.save_current_preset("Shared", true, true);
    bundle.set_num_filaments(3);
    bundle.set_filament_preset(0, "Shared");
    bundle.set_filament_preset(1, "_project/Shared");
    bundle.set_filament_preset(2, "_project/Shared");
    bundle.save_changes_for_preset("Copy", Preset::TYPE_FILAMENT, {}, false, true, "_project/Shared");
    CHECK(bundle.filament_presets == std::vector<std::string>{"Shared", "Copy", "Copy"});
    CHECK(presets.find_preset("Shared", false, true)->is_user());
    CHECK(presets.find_preset("_project/Shared", false, true)->is_project_embedded);
}

// Пересчёт совместимости и удаление последнего принтера не меняют библиотеку и проектные копии.
// Облачный обработчик заменён сборщиком имён: только удаляемый принтер может попасть в его очередь.
TEST_CASE("Удаление всех принтеров сохраняет библиотеку и проектные профили", "[PresetDeletion][Regression]")
{
    const auto type = GENERATE(Preset::TYPE_FILAMENT, Preset::TYPE_PRINT);
    const int compatibility = GENERATE(0, 1, 2, 3);
    const bool inherited_printer = GENERATE(false, true);
    CAPTURE(type, compatibility, inherited_printer);
    ScopedTemporaryDir temp;
    PresetBundle bundle;
    load_user_presets(bundle.printers, temp.path(), PRESET_PRINTER_NAME);
    for (const std::string name : {"Printer A", "Printer B"}) {
        bundle.printers.get_edited_preset().config.set_key_value("printer_model", new ConfigOptionString("Fork Test"));
        bundle.printers.save_current_preset(name, name != "Printer B" || !inherited_printer, false);
    }

    PresetCollection &profiles = type == Preset::TYPE_FILAMENT ? bundle.filaments : bundle.prints;
    load_user_presets(profiles, temp.path(), Preset::get_type_string(type));
    auto &config = profiles.get_edited_preset().config;
    const std::vector<std::string> compatible = compatibility == 1 ? std::vector<std::string>{"Printer A"} :
                                               compatibility == 2 ? std::vector<std::string>{"Printer A", "Printer B"} :
                                                                    std::vector<std::string>{};
    config.set_key_value("compatible_printers", new ConfigOptionStrings(compatible));
    config.set_key_value("compatible_printers_condition", new ConfigOptionString(compatibility == 3 ? "printer_model == \"Fork Test\"" : ""));
    profiles.save_current_preset("Library", true, false);
    profiles.get_selected_preset().setting_id = "library-cloud-id";
    profiles.get_selected_preset().save_info();
    profiles.select_preset_by_name("Library", true);
    profiles.save_current_preset("Child", false, false);
    profiles.save_current_preset("Project copy", true, true);
    const std::vector<Preset> before{*profiles.find_preset("Library"), *profiles.find_preset("Child"),
                                     *profiles.find_preset("_project/Project copy")};
    const auto library_json = saved_file_contents(before[0].file);
    const auto child_json = saved_file_contents(before[1].file);
    const auto info_path = fs::path(before[0].file).replace_extension(".info");
    const auto library_info = saved_file_contents(info_path);
    std::vector<std::string> cloud_deletions;
    // Удаляем потомка раньше родителя; проверяем как самостоятельный, так и унаследованный Printer B.
    for (const std::string name : {"Printer B", "Printer A"}) {
        bundle.printers.select_preset_by_name(name, true);
        REQUIRE(bundle.printers.delete_current_preset([&](const Preset &selected) { cloud_deletions.push_back(selected.name); }));
        bundle.update_compatible(PresetSelectCompatibleType::Always);
        for (const Preset &original : before) {
            const Preset *remaining = profiles.find_preset(original.name, false, true);
            REQUIRE(remaining != nullptr);
            CHECK(remaining->config.diff(original.config).empty());
            CHECK(remaining->setting_id == original.setting_id);
            CHECK(remaining->sync_info == original.sync_info);
            CHECK(remaining->is_project_embedded == original.is_project_embedded);
        }
        CHECK(saved_file_contents(before[0].file) == library_json);
        CHECK(saved_file_contents(before[1].file) == child_json);
        CHECK(saved_file_contents(info_path) == library_info);
    }
    CHECK(cloud_deletions == std::vector<std::string>{"Printer B", "Printer A"});
    CHECK(bundle.printers.only_default_printers());

    PresetBundle reloaded;
    auto &reloaded_profiles = type == Preset::TYPE_FILAMENT ? reloaded.filaments : reloaded.prints;
    load_user_presets(reloaded.printers, temp.path(), PRESET_PRINTER_NAME);
    load_user_presets(reloaded_profiles, temp.path(), Preset::get_type_string(type));
    CHECK(reloaded.printers.only_default_printers());
    REQUIRE(reloaded_profiles.find_preset("Library", false) != nullptr);
    REQUIRE(reloaded_profiles.find_preset("Child", false) != nullptr);
    reloaded.printers.get_edited_preset().config.set_key_value("printer_model", new ConfigOptionString("Fork Test"));
    reloaded.printers.save_current_preset("Printer A", true, false);
    reloaded.update_compatible(PresetSelectCompatibleType::Always);
    CHECK(reloaded_profiles.find_preset("Library")->is_compatible);
    CHECK(reloaded_profiles.find_preset("Child")->is_compatible);
    CHECK(saved_file_contents(before[0].file) == library_json);
    CHECK(saved_file_contents(before[1].file) == child_json);

    // Родитель защищён от удаления, а явное удаление дочерней записи не затрагивает соседей.
    reloaded_profiles.select_preset_by_name("Library", true);
    REQUIRE_FALSE(reloaded_profiles.delete_current_preset([&](const Preset &selected) { cloud_deletions.push_back(selected.name); }));
    CHECK(cloud_deletions.size() == 2);
    reloaded_profiles.select_preset_by_name("Child", true);
    REQUIRE(reloaded_profiles.delete_current_preset([&](const Preset &selected) { cloud_deletions.push_back(selected.name); }));
    CHECK(cloud_deletions.back() == "Child");
    CHECK_FALSE(fs::exists(before[1].file));
    CHECK(saved_file_contents(before[0].file) == library_json);
}

// Отвязка последнего профиля сохраняет устройство и сериализует пустой список, включая старое поле.
// Повторная загрузка и добавление новой связи не должны восстанавливать удалённую связь или терять настройки.
TEST_CASE("Физические принтеры не удаляются вместе с последним профилем", "[PresetDeletion][Regression]")
{
    ScopedTemporaryDir temp;
    fs::create_directory(temp.path() / "physical");
    PhysicalPrinterCollection printers(PhysicalPrinter::printer_options());
    PresetsConfigSubstitutions substitutions;
    printers.load_printers(temp.string(), "physical", substitutions, ForwardCompatibilitySubstitutionRule::Disable);
    for (const std::string name : {"Solo", "Shared", "Other"}) {
        PhysicalPrinter printer(name, printers.default_config());
        printer.config.set_key_value("print_host", new ConfigOptionString("http://192.0.2.1"));
        printer.add_preset(name == "Other" ? "Printer B" : "Printer A");
        if (name == "Shared")
            printer.add_preset("Printer B");
        printers.save_printer(printer);
    }
    const auto other_file = printers.find_printer("Other")->file;
    const auto other_contents = saved_file_contents(other_file);
    REQUIRE(printers.delete_preset_from_printers("Printer A"));
    REQUIRE(printers.find_printer("Solo") != nullptr);
    CHECK(printers.find_printer("Solo")->preset_names.empty());
    CHECK(printers.find_printer("Shared")->preset_names == std::set<std::string>{"Printer B"});
    CHECK(saved_file_contents(other_file) == other_contents);

    PhysicalPrinterCollection reloaded(PhysicalPrinter::printer_options());
    reloaded.load_printers(temp.string(), "physical", substitutions, ForwardCompatibilitySubstitutionRule::Disable);
    REQUIRE(substitutions.empty());
    REQUIRE(reloaded.find_printer("Solo") != nullptr);
    auto solo = *reloaded.find_printer("Solo");
    CHECK(solo.preset_names.empty());
    CHECK(solo.config.option<ConfigOptionStrings>("preset_names")->values.empty());
    CHECK(solo.config.option<ConfigOptionString>("preset_name")->value.empty());
    CHECK(solo.config.option<ConfigOptionString>("print_host")->value == "http://192.0.2.1");
    reloaded.select_printer("Solo");
    CHECK_FALSE(reloaded.has_selection());
    solo.add_preset("Printer B");
    reloaded.save_printer(solo);
    reloaded.select_printer("Solo");
    CHECK(reloaded.get_selected_printer_preset_name() == "Printer B");
    REQUIRE(reloaded.delete_selected_printer());
    CHECK(reloaded.find_printer("Solo") == nullptr);
    CHECK(reloaded.find_printer("Shared") != nullptr);
    CHECK(saved_file_contents(other_file) == other_contents);
}

// Пользовательская и проектная копии проходят реальное сохранение/detach и экспорт `.3mf`.
// Физический тип, признак поддержки и параметры экструзии сохраняются без специальных идентификаторов.
TEST_CASE("PLA поддержки сохраняется при detach и переносе через проект", "[PresetSaving][FilamentSupportType][3mf][Regression]")
{
    ScopedTemporaryDir temp;
    PresetBundle bundle;
    load_user_presets(bundle.filaments, temp.path(), PRESET_FILAMENT_NAME);
    add_system_filament(bundle);
    bundle.filaments.select_preset_by_name("System PLA", true);
    auto &config = bundle.filaments.get_edited_preset().config;
    config.set_key_value("filament_type", new ConfigOptionStrings{"PLA"});
    config.set_key_value("filament_is_support", new ConfigOptionBools{true});
    config.set_key_value("nozzle_temperature", new ConfigOptionInts{220});
    config.set_key_value("filament_flow_ratio", new ConfigOptionFloats{0.97});
    bundle.filaments.save_current_preset("PLA Support", true, false);
    bundle.filaments.save_current_preset("PLA Support", true, true);
    Preset embedded = bundle.filaments.get_selected_preset();
    CHECK(embedded.inherits().empty());
    ScopedTemporaryFile archive(".3mf");
    ImportedProjectData imported;
    DynamicPrintConfig loaded_config;
    auto project_config = DynamicPrintConfig::full_print_config();
    project_config.apply(embedded.config);
    roundtrip_project(archive.string(), project_config, {&embedded}, loaded_config, imported);
    REQUIRE(imported.presets.size() == 1);
    std::string displayed;
    auto &loaded = imported.presets.front()->config;
    CHECK(loaded.get_filament_type(displayed) == "PLA");
    CHECK(displayed == "PLA");
    CHECK(loaded.option<ConfigOptionBools>("filament_is_support")->get_at(0));
    CHECK(loaded.option<ConfigOptionInts>("nozzle_temperature")->get_at(0) == 220);
    CHECK_THAT(loaded.option<ConfigOptionFloats>("filament_flow_ratio")->get_at(0), Catch::Matchers::WithinAbs(0.97, 1e-9));
    PresetBundle reloaded;
    load_user_presets(reloaded.filaments, temp.path(), PRESET_FILAMENT_NAME);
    REQUIRE(reloaded.filaments.find_preset("PLA Support", false) != nullptr);
    auto &user = reloaded.filaments.find_preset("PLA Support")->config;
    CHECK(user.get_filament_type(displayed) == "PLA");
    CHECK(user.option<ConfigOptionBools>("filament_is_support")->get_at(0));
}

// Старая пользовательская копия без переопределённого типа получает явный PLA-S от обновлённой базы.
// Намеренное переопределение обратно в обычный PLA при этом не отменяется наследованием.
TEST_CASE("Пользовательский наследник получает явный тип специального материала из базы", "[PresetSaving][FilamentSupportType][Regression]")
{
    ScopedTemporaryDir temp;
    PresetBundle original;
    add_system_filament(original);
    original.filaments.find_preset("System PLA")->config.set_key_value("filament_is_support", new ConfigOptionBools{true});
    load_user_presets(original.filaments, temp.path(), PRESET_FILAMENT_NAME);
    original.filaments.select_preset_by_name("System PLA", true);
    original.filaments.save_current_preset("Support copy", false, false);

    PresetBundle updated;
    add_system_filament(updated);
    auto &base = updated.filaments.find_preset("System PLA")->config;
    base.set_key_value("filament_type", new ConfigOptionStrings{"PLA-S"});
    base.set_key_value("filament_is_support", new ConfigOptionBools{true});
    load_user_presets(updated.filaments, temp.path(), PRESET_FILAMENT_NAME);
    updated.filaments.select_preset_by_name("Support copy", true);
    auto &copy = updated.filaments.get_edited_preset();
    REQUIRE(copy.inherits() == "System PLA");
    std::string displayed;
    CHECK(copy.get_filament_type(displayed) == "PLA-S");
    CHECK(displayed == "Sup.PLA");
    copy.config.set_key_value("filament_type", new ConfigOptionStrings{"PLA"});
    updated.filaments.save_current_preset("Support copy", false, false);
    CHECK(updated.filaments.get_selected_preset().config.get_filament_type(displayed) == "PLA");
    CHECK(displayed == "PLA");
}
