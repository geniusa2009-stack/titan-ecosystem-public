// TITAN material system tests. Original TITAN code.
#include "titan/material.h"
#include "titan_test.h"

using namespace titan;

namespace {
Json rgb(double r, double g, double b) { return Json(Json::Array{Json(r), Json(g), Json(b)}); }

MaterialParam param(const char* name, ParamType t, Json v) {
    MaterialParam p;
    p.name = name;
    p.type = t;
    p.value = std::move(v);
    return p;
}

Material surface() {
    Material m;
    m.name = "Surface";
    MaterialParam rough = param("roughness", ParamType::Float, 0.5);
    rough.min = 0.0;
    rough.max = 1.0;
    m.params = {param("baseColor", ParamType::Color, rgb(1, 1, 1)), rough, param("twoSided", ParamType::Bool, false),
                param("albedoMap", ParamType::Texture, "")};
    return m;
}

Material instance(const char* name, const char* parent, std::map<std::string, Json> overrides) {
    Material m;
    m.name = name;
    m.parent = parent;
    m.overrides = std::move(overrides);
    return m;
}
}  // namespace

TEST("material: instances inherit and override typed parameters") {
    MaterialLibrary lib;
    lib.add(surface());
    std::map<std::string, Json> redOverrides;
    redOverrides["baseColor"] = rgb(1, 0, 0);
    lib.add(instance("Red", "Surface", redOverrides));
    std::map<std::string, Json> shinyOverrides;
    shinyOverrides["roughness"] = 0.1;
    lib.add(instance("ShinyRed", "Red", shinyOverrides));
    const auto v = lib.resolve("ShinyRed");
    CHECK(v.size() == 4);
    CHECK(v.at("baseColor") == rgb(1, 0, 0));
    CHECK(v.at("roughness").asNumber() == 0.1);
    CHECK(v.at("twoSided").asBool() == false);
    CHECK(lib.resolve("Surface").at("roughness").asNumber() == 0.5);
    lib.setOverride("Red", "roughness", 0.9);
    CHECK(lib.resolve("Red").at("roughness").asNumber() == 0.9);
    CHECK(lib.resolve("ShinyRed").at("roughness").asNumber() == 0.1);  // the child's override still wins
    CHECK(lib.paramOf("ShinyRed", "roughness")->type == ParamType::Float);
    CHECK(lib.paramOf("ShinyRed", "metallic") == nullptr);
}

TEST("material: invalid definitions and overrides change nothing") {
    MaterialLibrary lib;
    lib.add(surface());
    CHECK_THROWS(lib.add(surface()));  // duplicate
    std::map<std::string, Json> wrongType;
    wrongType["roughness"] = "rough";
    CHECK_THROWS(lib.add(instance("Bad1", "Surface", wrongType)));
    std::map<std::string, Json> outOfRange;
    outOfRange["roughness"] = 1.5;
    CHECK_THROWS(lib.add(instance("Bad2", "Surface", outOfRange)));
    std::map<std::string, Json> unknownParam;
    unknownParam["metallic"] = 1.0;
    CHECK_THROWS(lib.add(instance("Bad3", "Surface", unknownParam)));
    std::map<std::string, Json> negativeColor;
    negativeColor["baseColor"] = rgb(-1, 0, 0);
    CHECK_THROWS(lib.add(instance("Bad4", "Surface", negativeColor)));
    CHECK_THROWS(lib.add(instance("Orphan", "Missing", {})));
    Material instWithParams = instance("Bad5", "Surface", {});
    instWithParams.params.push_back(param("extra", ParamType::Float, 1.0));
    CHECK_THROWS(lib.add(instWithParams));
    Material dupParam = surface();
    dupParam.name = "Dup";
    dupParam.params.push_back(dupParam.params[0]);
    CHECK_THROWS(lib.add(dupParam));
    Material badDefault = surface();
    badDefault.name = "BadDefault";
    badDefault.params[1].value = 2.0;
    CHECK_THROWS(lib.add(badDefault));
    Material badName = surface();
    badName.name = "has space";
    CHECK_THROWS(lib.add(badName));
    CHECK(lib.size() == 1);
    lib.add(instance("Plain", "Surface", {}));
    CHECK_THROWS(lib.setOverride("Plain", "twoSided", 1));
    CHECK_THROWS(lib.setOverride("Surface", "twoSided", true));  // bases have no overrides
    CHECK(lib.find("Plain")->overrides.empty());
}

TEST("material: instance chains are depth limited") {
    MaterialLibrary lib;
    lib.add(surface());
    std::string parent = "Surface";
    for (int i = 1; i <= 16; ++i) {
        const std::string name = "L" + std::to_string(i);
        lib.add(instance(name.c_str(), parent.c_str(), {}));
        parent = name;
    }
    CHECK_THROWS(lib.add(instance("L17", "L16", {})));
    CHECK(lib.resolve("L16").size() == 4);
}

TEST("material: json round trip keeps parents before children") {
    MaterialLibrary lib;
    lib.add(surface());
    std::map<std::string, Json> ov;
    ov["albedoMap"] = "textures/brick.png";
    ov["twoSided"] = true;
    lib.add(instance("Brick", "Surface", ov));
    const Json j = lib.toJson();
    const MaterialLibrary back = MaterialLibrary::fromJson(Json::parse(j.dump()));
    CHECK(back.toJson() == j);
    CHECK(back.resolve("Brick") == lib.resolve("Brick"));
    CHECK(back.paramOf("Surface", "roughness")->max.value() == 1.0);
    Json reordered = j;
    auto& arr = reordered["materials"].asArray();
    std::swap(arr[0], arr[1]);
    CHECK_THROWS(MaterialLibrary::fromJson(reordered));  // child before parent
    Json badType = j;
    badType["materials"].asArray()[0]["params"].asArray()[0]["type"] = "vector";
    CHECK_THROWS(MaterialLibrary::fromJson(badType));
}

TITAN_TEST_MAIN()
