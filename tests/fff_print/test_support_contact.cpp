// Проверяет роли, расход и скорость линий модели над фактически напечатанной поддержкой без зазора.
// Геометрия опоры извлекается из экструзий поддержки, независимо от кэша производственного кода.
#include <catch2/catch_all.hpp>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/GCode/ExtrusionProcessor.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Layer.hpp"
#include "test_helpers.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::Test;

// Собирает невладеющие ссылки на пути всех вложенных сущностей; владелец `Print` остаётся жив.
static void collect_contact_test_paths(const ExtrusionEntity &entity, std::vector<const ExtrusionPath *> &out)
{
    if (const auto *path = dynamic_cast<const ExtrusionPath *>(&entity)) {
        out.push_back(path);
    } else if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(&entity)) {
        for (const ExtrusionPath &path : loop->paths)
            out.push_back(&path);
    } else if (const auto *multi = dynamic_cast<const ExtrusionMultiPath *>(&entity)) {
        for (const ExtrusionPath &path : multi->paths)
            out.push_back(&path);
    } else if (const auto *collection = dynamic_cast<const ExtrusionEntityCollection *>(&entity)) {
        for (const ExtrusionEntity *child : collection->entities)
            collect_contact_test_paths(*child, out);
    } else {
        FAIL("Неизвестная сущность экструзии в проверке контакта");
    }
}

// Объединяет ширину реальных линий поддержки, верх которых совпадает с низом слоя модели.
static Polygons printed_support_contact(const PrintObject &object, const Layer &layer)
{
    Polygons contact;
    for (const SupportLayer *support : object.support_layers()) {
        if (std::abs(support->print_z - layer.bottom_z()) < EPSILON)
            support->support_fills.polygons_covered_by_width(contact, float(SCALED_EPSILON));
    }
    return union_(contact);
}

// Возвращает длину центральных линий внутри опоры в миллиметрах, не учитывая перемещения без экструзии.
static double length_in_contact(const ExtrusionPath &path, const Polygons &contact)
{
    double length = 0.;
    for (const Polyline &line : intersection_pl(Polylines{path.polyline.to_polyline()}, contact))
        length += unscale<double>(line.length());
    return length;
}

// Задаёт геометрически плотный контакт и все настройки, от которых зависит распознавание нависаний.
static DynamicPrintConfig support_contact_config(const char *wall_generator, bool bridge_no_support)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"layer_height", "0.2"}, {"initial_layer_print_height", "0.2"},
        {"nozzle_diameter", "0.4"}, {"line_width", "0.4"},
        {"outer_wall_line_width", "0.4"}, {"inner_wall_line_width", "0.4"},
        {"internal_solid_infill_line_width", "0.4"},
        {"wall_loops", "3"}, {"wall_generator", wall_generator},
        {"detect_overhang_wall", "1"}, {"extra_perimeters_on_overhangs", "0"},
        {"thick_bridges", "0"}, {"bridge_flow", "0.5"},
        {"enable_support", "1"}, {"support_type", "normal(auto)"},
        {"support_top_z_distance", "0"}, {"support_bottom_z_distance", "0"},
        {"support_interface_top_layers", "3"}, {"support_interface_spacing", "0"},
        {"support_interface_pattern", "rectilinear"},
        {"support_object_xy_distance", "0"}, {"support_threshold_angle", "60"},
        {"support_remove_small_overhang", "0"}, {"support_on_build_plate_only", "1"},
        {"bridge_no_support", bridge_no_support ? "1" : "0"},
        {"sparse_infill_density", "15%"}
    });
    return config;
}

// Нижняя часть сферы расширяется между слоями, но плотная поддержка делает её периметры опёртыми.
// Проверка требует реальных пересечений с опорой и отвергает мостовую роль внутри этих пересечений.
TEST_CASE("Периметры сферы над поддержкой без зазора печатаются обычными стенками", "[SupportContact][Regression]")
{
    const char *wall_generator = GENERATE("arachne", "classic");
    const bool bridge_no_support = GENERATE(false, true);
    const bool extra_perimeters = GENERATE(false, true);
    CAPTURE(wall_generator, bridge_no_support, extra_perimeters);

    bool cancel_called = false;
    Model model;
    Print print;
    DynamicPrintConfig config = support_contact_config(wall_generator, bridge_no_support);
    config.set_deserialize_strict("extra_perimeters_on_overhangs", extra_perimeters ? "1" : "0");
    init_print({make_sphere(8.)}, print, model, config);
    print.set_cancel_callback([&cancel_called]() { cancel_called = true; });
    print.process();
    CHECK_FALSE(cancel_called);

    const PrintObject &object = *print.objects().front();
    REQUIRE_FALSE(object.support_layers().empty());
    double supported_length = 0.;
    double bridging_length = 0.;
    for (const Layer *layer : object.layers()) {
        // Небольшое сужение исключает касание самой границы контактной линии из строгой проверки.
        Polygons contact = offset(printed_support_contact(object, *layer), -scaled<double>(0.02));
        if (contact.empty())
            continue;
        for (const LayerRegion *region : layer->regions()) {
            std::vector<const ExtrusionPath *> paths;
            collect_contact_test_paths(region->perimeters, paths);
            for (const ExtrusionPath *path : paths) {
                const double length = length_in_contact(*path, contact);
                supported_length += length;
                if (path->role() == erOverhangPerimeter)
                    bridging_length += length;
                else if (length > 0.01) {
                    const Flow ordinary_flow(path->width, path->height, 0.4f);
                    CHECK_THAT(path->mm3_per_mm, Catch::Matchers::WithinAbs(ordinary_flow.mm3_per_mm(), 1e-6));
                }
            }
        }
    }
    REQUIRE(supported_length > 1.);
    CHECK(bridging_length < 0.01);
}

// Создаёт балку на двух модельных стойках и отдельный ручной остров поддержки в середине пролёта.
// Объём `SUPPORT_ENFORCER` пересекает низ балки, но не входит в печатаемую геометрию модели.
static void init_supported_bridge(Print &print, Model &model, const DynamicPrintConfig &config)
{
    TriangleMesh bridge = make_cube(60., 12., 1.);
    bridge.translate(0., 0., 6.);
    bridge.merge(make_cube(8., 12., 6.));
    TriangleMesh right = make_cube(8., 12., 6.);
    right.translate(52., 0., 0.);
    bridge.merge(right);
    // Не поворачиваем балку размещением: два пролёта должны оставаться слева и справа по X.
    init_print(std::vector<TriangleMesh>{std::move(bridge)}, print, model, config, nullptr, false);

    TriangleMesh island = make_cube(12., 14., 6.1);
    island.translate(24., -1., 0.);
    ModelVolume *enforcer = model.objects.front()->add_volume(std::move(island));
    enforcer->set_type(ModelVolumeType::SUPPORT_ENFORCER);
    print.apply(model, config);
}

// Находит первый слой балки над пролётом. Его нижняя граница задана геометрией теста, а не порядковым номером.
static const Layer &bridge_contact_layer(const PrintObject &object)
{
    auto layer = std::find_if(object.layers().begin(), object.layers().end(),
        [](const Layer *candidate) { return std::abs(candidate->bottom_z() - 6.) < EPSILON; });
    REQUIRE(layer != object.layers().end());
    return **layer;
}

// Измеряет линии выбранной роли в области острова, чтобы проверять конечные экструзии, а не только тип поверхности.
static double fill_role_length(const Layer &layer, ExtrusionRole role, const Polygons &area)
{
    double length = 0.;
    for (const LayerRegion *region : layer.regions()) {
        std::vector<const ExtrusionPath *> paths;
        collect_contact_test_paths(region->fills, paths);
        for (const ExtrusionPath *path : paths)
            if (path->role() == role)
                length += length_in_contact(*path, area);
    }
    return length;
}

// Ручная опора разделяет пролёт на два моста независимо от запрета автоматической поддержки мостов.
// Центр острова лежит дальше штатных зон закрепления; сами мосты должны заходить на реальную опору.
TEST_CASE("Ручной остров поддержки разделяет мост и сохраняет обычное заполнение над опорой", "[SupportContact][Regression]")
{
    const char *wall_generator = GENERATE("arachne", "classic");
    const char *support_type = GENERATE("normal(manual)", "tree(manual)");
    CAPTURE(wall_generator, support_type);
    DynamicPrintConfig config = support_contact_config(wall_generator, true);
    config.set_deserialize_strict({{"support_type", support_type}, {"bottom_surface_pattern", "concentric"}});

    Model model;
    Print print;
    init_supported_bridge(print, model, config);
    print.process();

    const PrintObject &object = *print.objects().front();
    const Layer &layer = bridge_contact_layer(object);
    const Polygons contact = printed_support_contact(object, layer);
    REQUIRE_FALSE(contact.empty());
    const BoundingBox contact_bounds = get_extents(contact);
    const Vec3d model_volume_offset = model.objects.front()->volumes.front()->get_offset();
    INFO("Контакт: " << contact.size() << " контуров, " << unscale<double>(contact_bounds.size().x()) << " x "
         << unscale<double>(contact_bounds.size().y()) << " мм; смещение объёма модели: "
         << model_volume_offset.x() << ", " << model_volume_offset.y() << ", " << model_volume_offset.z());
    // Остров шириной 12 мм оставляет внутреннюю область вне закрепления пролётов по обе стороны.
    const Polygons core = offset(contact, -scaled<double>(2.5));
    REQUIRE_FALSE(core.empty());
    CHECK(fill_role_length(layer, erBottomSurface, core) > 1.);
    CHECK(fill_role_length(layer, erBridgeInfill, core) < 0.01);

    for (const LayerRegion *region : layer.regions()) {
        std::vector<const ExtrusionPath *> paths;
        collect_contact_test_paths(region->fills, paths);
        for (const ExtrusionPath *path : paths) {
            if (path->role() == erBottomSurface && length_in_contact(*path, core) > 0.01) {
                const Flow ordinary_flow(path->width, path->height, 0.4f);
                CHECK_THAT(path->mm3_per_mm, Catch::Matchers::WithinAbs(ordinary_flow.mm3_per_mm(), 1e-6));
            }
        }
    }

    bool left_anchored = false;
    bool right_anchored = false;
    for (const LayerRegion *region : layer.regions()) {
        for (const Surface &surface : region->fill_surfaces.surfaces) {
            if (surface.surface_type != stBottomBridge)
                continue;
            const BoundingBox bounds = get_extents(surface.expolygon);
            const bool touches_support = !intersection_ex(ExPolygons{surface.expolygon}, contact).empty();
            if (bounds.min.x() < contact_bounds.min.x() - scaled<double>(2.))
                left_anchored |= touches_support;
            if (bounds.max.x() > contact_bounds.max.x() + scaled<double>(2.))
                right_anchored |= touches_support;
        }
    }
    CHECK(left_anchored);
    CHECK(right_anchored);
}

// Изменение зазора на том же `Print` должно удалять прежний контакт, а обратное изменение — восстанавливать его.
TEST_CASE("Повторная нарезка не использует устаревшую опору после изменения Z-зазора", "[SupportContact][Regression]")
{
    DynamicPrintConfig config = support_contact_config("arachne", true);
    config.set_deserialize_strict("support_type", "normal(manual)");
    Model model;
    Print print;
    init_supported_bridge(print, model, config);
    print.process();

    const Polygons core = offset(printed_support_contact(*print.objects().front(),
        bridge_contact_layer(*print.objects().front())), -scaled<double>(2.5));
    REQUIRE_FALSE(core.empty());
    REQUIRE(fill_role_length(bridge_contact_layer(*print.objects().front()), erBottomSurface, core) > 1.);

    config.set_deserialize_strict("support_top_z_distance", "0.2");
    print.apply(model, config);
    print.process();
    const Layer &separated = bridge_contact_layer(*print.objects().front());
    CHECK(fill_role_length(separated, erBottomSurface, core) < 0.01);
    CHECK(fill_role_length(separated, erBridgeInfill, core) > 1.);

    config.set_deserialize_strict("support_top_z_distance", "0");
    print.apply(model, config);
    print.process();
    const Layer &touching = bridge_contact_layer(*print.objects().front());
    CHECK(fill_role_length(touching, erBottomSurface, core) > 1.);
    CHECK(fill_role_length(touching, erBridgeInfill, core) < 0.01);
}

// Оценщик динамического замедления должен видеть ту же опору, что генераторы линий.
// Проба лежит посередине пролёта: нижнего модельного слоя под ней нет, есть только ручной остров.
TEST_CASE("Контакт поддержки исключает мостовое замедление и недостаточное перекрытие", "[SupportContact][Regression]")
{
    DynamicPrintConfig config = support_contact_config("arachne", true);
    config.set_deserialize_strict("support_type", "normal(manual)");
    Model model;
    Print print;
    init_supported_bridge(print, model, config);
    print.process();

    const PrintObject &object = *print.objects().front();
    const Layer &layer = bridge_contact_layer(object);
    const Polygons contact = printed_support_contact(object, layer);
    REQUIRE_FALSE(contact.empty());
    const Point center = get_extents(contact).center();
    const Flow ordinary_flow(0.4f, 0.2f, 0.4f);
    ExtrusionPath probe(erPerimeter, ordinary_flow.mm3_per_mm(), ordinary_flow.width(), ordinary_flow.height());
    probe.polyline.points = {Point3(center.x() - scaled<coord_t>(0.5), center.y(), coord_t(0)),
                             Point3(center.x() + scaled<coord_t>(0.5), center.y(), coord_t(0))};
    REQUIRE(length_in_contact(probe, offset(contact, -scaled<double>(0.5))) > 0.99);
    REQUIRE(length_in_contact(probe, to_polygons(layer.lower_layer->lslices)) < 0.01);

    ExtrusionQualityEstimator estimator;
    estimator.set_current_object(&object);
    estimator.prepare_for_new_layer(&object, layer.lower_layer);
    estimator.prepare_for_new_layer(&object, &layer);
    const ConfigOptionPercents overlaps{100., 0.};
    const ConfigOptionFloatsOrPercents speeds{{100., false}, {10., false}};
    const std::vector<ProcessedPoint> points = estimator.estimate_extrusion_quality(
        probe, overlaps, speeds, 100.f, 100.f, false);
    REQUIRE_FALSE(points.empty());
    for (const ProcessedPoint &point : points) {
        CHECK_THAT(point.speed, Catch::Matchers::WithinAbs(100., 1e-6));
        CHECK(point.overlap >= 1.f);
    }
}

// Реальный экспорт балки с ручным островом проверяет оба предела первого слоя и отсутствие ускорения.
// Другие причины замедления отключены явно; свободные пролёты и следующий слой сохраняют свои скорости.
TEST_CASE("Над поддержкой скорость первого слоя ограничивает только контактные участки модели", "[SupportContact][SupportContactSpeed][Regression]")
{
    const char *wall_generator = GENERATE("arachne", "classic");
    const double ordinary_speed = GENERATE(11., 17.5, 80.);
    CAPTURE(wall_generator, ordinary_speed);
    DynamicPrintConfig config = support_contact_config(wall_generator, true);
    config.set_deserialize_strict({
        {"support_type", "normal(manual)"}, {"initial_layer_speed", "17"}, {"initial_layer_infill_speed", "31"},
        {"outer_wall_speed", std::to_string(ordinary_speed)}, {"inner_wall_speed", std::to_string(ordinary_speed)},
        {"small_perimeter_speed", "100%"}, {"bridge_speed", "56"}, {"slow_down_layers", "1"},
        {"enable_overhang_speed", "0"}, {"enable_overhang_bridge_fan", "0"},
        {"slow_down_for_layer_cooling", "0"}, {"filament_max_volumetric_speed", "1000"},
        {"enable_arc_fitting", "0"}, {"gcode_flavor", "marlin"}, {"use_relative_e_distances", "0"}
    });
    Model model;
    Print print;
    init_supported_bridge(print, model, config);
    print.process();

    const PrintObject &object = *print.objects().front();
    const Layer &layer = bridge_contact_layer(object);
    const Polygons contact = printed_support_contact(object, layer);
    REQUIRE_FALSE(contact.empty());
    const Polygons core = offset(contact, -scaled<double>(0.02));
    const Polygons outside = diff(to_polygons(layer.lslices), offset(contact, scaled<double>(1.)));
    const Point shift = object.instances().front().shift;
    double wall_length = 0.;
    double fill_length = 0.;
    double bridge_length = 0.;
    double upper_wall_length = 0.;
    double max_wall_speed = 0.;
    double max_fill_speed = 0.;
    double max_bridge_speed = 0.;
    double max_upper_wall_speed = 0.;
    std::string role;
    GCodeReader reader;
    reader.apply_config(config);
    const std::string output = gcode(print);
    const std::string &role_tag = GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Role);
    reader.parse_buffer(output, [&](GCodeReader &state, const GCodeReader::GCodeLine &line) {
        const std::string_view comment = line.comment();
        if (comment.substr(0, role_tag.size()) == role_tag)
            role = comment.substr(role_tag.size());
        if (!line.extruding(state) || line.dist_XY(state) < 0.005)
            return;
        const bool wall = role == "Outer wall" || role == "Inner wall";
        const double speed = line.new_F(state) / MM_PER_MIN;
        if (std::abs(line.new_Z(state) - (layer.print_z + layer.height)) < 0.001 && wall) {
            upper_wall_length += line.dist_XY(state);
            max_upper_wall_speed = std::max(max_upper_wall_speed, speed);
        }
        if (std::abs(line.new_Z(state) - layer.print_z) >= 0.001)
            return;
        const Polyline move{Point::new_scale(state.x(), state.y()) - shift,
                            Point::new_scale(line.new_X(state), line.new_Y(state)) - shift};
        double on_support = 0.;
        for (const Polyline &part : intersection_pl(Polylines{move}, core))
            on_support += unscale<double>(part.length());
        if (on_support > 0.01 && wall) {
            wall_length += on_support;
            max_wall_speed = std::max(max_wall_speed, speed);
        } else if (on_support > 0.01 && role == "Bottom surface") {
            fill_length += on_support;
            max_fill_speed = std::max(max_fill_speed, speed);
        }
        if (role == "Bridge" && !intersection_pl(Polylines{move}, outside).empty()) {
            bridge_length += line.dist_XY(state);
            max_bridge_speed = std::max(max_bridge_speed, speed);
        }
    });
    INFO("Длины: стенки=" << wall_length << ", заполнение=" << fill_length << ", мост=" << bridge_length
         << ", следующий слой=" << upper_wall_length << "; метка=" << role_tag);
    REQUIRE(wall_length > 1.);
    REQUIRE(fill_length > 1.);
    REQUIRE(bridge_length > 1.);
    REQUIRE(upper_wall_length > 1.);
    CHECK_THAT(max_wall_speed, Catch::Matchers::WithinAbs(std::min(ordinary_speed, 17.), 0.001));
    CHECK_THAT(max_fill_speed, Catch::Matchers::WithinAbs(31., 0.001));
    CHECK_THAT(max_bridge_speed, Catch::Matchers::WithinAbs(56., 0.001));
    CHECK_THAT(max_upper_wall_speed, Catch::Matchers::WithinAbs(ordinary_speed, 0.001));
}

// Оба конца линии находятся вне опоры: замедляется только середина, в обоих направлениях прохода.
// Разделение сохраняет высоту, исходную длину для компенсации расхода и решение об охлаждении.
TEST_CASE("Промежуточный остров замедляет только лежащую над ним часть линии", "[SupportContact][SupportContactSpeed][Regression]")
{
    const bool reverse = GENERATE(false, true);
    Model model;
    Print print;
    init_print({make_cube(1., 1., 1.)}, print, model, support_contact_config("arachne", true));
    PrintObject &object = *print.get_object(0);
    // Объект владеет слоями; для проверки разделения достаточно явно заданной контактной геометрии.
    Layer &layer = *object.add_layer(0, 0.2, 0.4, 0.3);
    layer.support_contact = {ExPolygon{Polygon{Point::new_scale(-1., -1.), Point::new_scale(1., -1.),
                                               Point::new_scale(1., 1.), Point::new_scale(-1., 1.)}}};
    ExtrusionPath path(erPerimeter, 0.08, 0.4f, 0.2f);
    path.polyline.points = {Point3::new_scale(-5., 0., 0.), Point3::new_scale(5., 0., 0.2)};
    if (reverse)
        std::reverse(path.polyline.points.begin(), path.polyline.points.end());
    std::vector<ProcessedPoint> points{{path.polyline.points.front(), 80.f, 0.2f},
                                       {path.polyline.points.back(), 80.f, 0.9f}};
    ExtrusionQualityEstimator estimator;
    REQUIRE(estimator.limit_speed_over_support(layer, path, 80.f, 17.f, points));
    REQUIRE(points.size() == 4);
    CHECK(points.front().p == path.polyline.points.front());
    CHECK(points.back().p == path.polyline.points.back());
    CHECK_THAT(points[0].speed, Catch::Matchers::WithinAbs(80., 1e-6));
    CHECK_THAT(points[1].speed, Catch::Matchers::WithinAbs(17., 1e-6));
    CHECK_THAT(points[2].speed, Catch::Matchers::WithinAbs(80., 1e-6));
    CHECK_THAT(unscale<double>(points[1].p.x()), Catch::Matchers::WithinAbs(reverse ? 1. : -1., 1e-6));
    CHECK_THAT(unscale<double>(points[2].p.x()), Catch::Matchers::WithinAbs(reverse ? -1. : 1., 1e-6));
    CHECK_THAT(unscale<double>(points[1].p.z()), Catch::Matchers::WithinAbs(reverse ? 0.12 : 0.08, 1e-6));
    CHECK_THAT(unscale<double>(points[2].p.z()), Catch::Matchers::WithinAbs(reverse ? 0.08 : 0.12, 1e-6));
    for (size_t i = 0; i + 1 < points.size(); ++i) {
        CHECK_THAT(points[i].flow_length, Catch::Matchers::WithinAbs(unscale<double>(path.polyline.length()), 1e-6));
        CHECK(std::max(points[i].overlap, points[i + 1].overlap) > 0.75f);
    }

    // Геометрия следующего слоя не содержит контакта; кэш прежней опоры не должен применяться к нему.
    Layer &next = *object.add_layer(1, 0.2, 0.6, 0.5);
    std::vector<ProcessedPoint> unchanged;
    CHECK_FALSE(estimator.limit_speed_over_support(next, path, 80.f, 17.f, unchanged));
    CHECK(unchanged.empty());
    CHECK_FALSE(estimator.limit_speed_over_support(layer, path, 11.f, 17.f, unchanged));
    CHECK(unchanged.empty());
}
