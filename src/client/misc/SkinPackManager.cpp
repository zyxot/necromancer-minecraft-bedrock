#include "pch.h"
#include "SkinPackManager.h"
#include "util/Util.h"
#include "util/Logger.h"

#include <Shellapi.h>
#include <fstream>
#include <random>
#include <sstream>

namespace {
    bool writeFile(std::filesystem::path const& path, std::string const& content) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) return false;
        out << content;
        return out.good();
    }

    std::string readFile(std::filesystem::path const& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in.is_open()) return {};
        std::stringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    std::string sanitizeName(std::string const& in) {
        std::string out;
        for (char c : in) {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-') {
                out.push_back(c);
            } else {
                out.push_back('_');
            }
        }
        return out;
    }

    std::string toLower(std::string s) {
        for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        return s;
    }

    bool readPngSize(std::filesystem::path const& path, uint32_t& width, uint32_t& height) {
        std::ifstream in(path, std::ios::binary);
        if (!in.is_open()) return false;

        unsigned char header[24] {};
        in.read(reinterpret_cast<char*>(header), sizeof(header));
        if (in.gcount() < static_cast<std::streamsize>(sizeof(header))) return false;

        static constexpr unsigned char magic[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
        for (int i = 0; i < 8; i++) {
            if (header[i] != magic[i]) return false;
        }
        if (header[12] != 'I' || header[13] != 'H' || header[14] != 'D' || header[15] != 'R') return false;

        width = (static_cast<uint32_t>(header[16]) << 24) | (static_cast<uint32_t>(header[17]) << 16) |
                (static_cast<uint32_t>(header[18]) << 8) | static_cast<uint32_t>(header[19]);
        height = (static_cast<uint32_t>(header[20]) << 24) | (static_cast<uint32_t>(header[21]) << 16) |
                 (static_cast<uint32_t>(header[22]) << 8) | static_cast<uint32_t>(header[23]);
        return width > 0 && height > 0 && width <= 4096 && height <= 4096;
    }

    std::string makeUuid() {
        static std::mt19937_64 rng { std::random_device {}() };
        auto part = [&](int len) {
            std::stringstream ss;
            ss << std::hex;
            for (int i = 0; i < len; i++) {
                if (i == 0) {
                    ss << (1 + rng() % 9);
                } else {
                    ss << (rng() % 16);
                }
            }
            return ss.str();
        };
        return part(8) + "-" + part(4) + "-4" + part(3).substr(1) + "-8" + part(3).substr(1) + "-" + part(12);
    }

    void loadOrCreateUuids(std::filesystem::path const& folder, std::string& headerUuid, std::string& moduleUuid) {
        std::filesystem::path uuidFile = folder / "uuid.txt";
        std::ifstream in(uuidFile);
        if (in.is_open() && std::getline(in, headerUuid) && std::getline(in, moduleUuid) && headerUuid.size() == 36 &&
            moduleUuid.size() == 36) {
            return;
        }

        headerUuid = makeUuid();
        moduleUuid = makeUuid();
        writeFile(uuidFile, headerUuid + "\n" + moduleUuid + "\n");
    }

    constexpr char const* packHeaderName = "Necromancer Skins";

    void registerKnownPack(std::filesystem::path const& mojang, std::filesystem::path const& packDir,
                           std::string const& uuid) {
        std::error_code ec;
        std::filesystem::path reg = mojang / "minecraftpe" / "valid_known_packs.json";
        std::filesystem::create_directories(reg.parent_path(), ec);

        std::string raw = readFile(reg);
        bool hasBom = raw.size() >= 3 && static_cast<unsigned char>(raw[0]) == 0xEF &&
                      static_cast<unsigned char>(raw[1]) == 0xBB && static_cast<unsigned char>(raw[2]) == 0xBF;
        json doc = json::parse(hasBom ? raw.substr(3) : raw, nullptr, false);
        if (!doc.is_array()) doc = json::array();

        std::string pathStr = packDir.string();
        bool updated = false;
        for (auto& entry : doc) {
            if (!entry.is_object()) continue;
            if (entry.value("uuid", std::string()) != uuid && entry.value("path", std::string()) != pathStr) continue;

            entry["file_system"] = "RawPath";
            entry["path"] = pathStr;
            entry["uuid"] = uuid;
            entry["version"] = "1.0.0";
            if (!entry.contains("file_version")) entry["file_version"] = 2;
            updated = true;
            break;
        }
        if (!updated) {
            doc.push_back(json { { "file_system", "RawPath" },
                                 { "path", pathStr },
                                 { "uuid", uuid },
                                 { "version", "1.0.0" },
                                 { "file_version", 2 } });
        }

        writeFile(reg, (hasBom ? "\xEF\xBB\xBF" : "") + doc.dump(1));
    }

    std::vector<std::string> collectGeometryIds(json const& doc) {
        std::vector<std::string> ids;
        if (!doc.is_object()) return ids;

        if (auto modern = doc.find("minecraft:geometry"); modern != doc.end() && modern->is_array()) {
            for (auto const& model : *modern) {
                if (!model.is_object()) continue;
                auto desc = model.find("description");
                if (desc == model.end() || !desc->is_object()) continue;
                auto ident = desc->find("identifier");
                if (ident == desc->end() || !ident->is_string()) continue;
                ids.push_back(ident->get<std::string>());
            }
            return ids;
        }

        for (auto const& [key, value] : doc.items()) {
            if (key.rfind("geometry.", 0) == 0) ids.push_back(key);
        }
        return ids;
    }

    std::filesystem::path findGeometryFile(std::filesystem::path const& folder, std::string const& stem) {
        std::error_code ec;
        for (char const* suffix : { ".geo.json", ".json" }) {
            std::filesystem::path candidate = folder / (stem + suffix);
            if (std::filesystem::exists(candidate, ec)) return candidate;
        }
        return {};
    }

    std::string const readmeText =
        "Necromancer skins folder\n"
        "\n"
        "1. Drop skin PNG files in here (64x64, 64x32 or HD).\n"
        "2. Open the client menu > Misc > Skins and press Sync.\n"
        "3. Restart Minecraft once, then pick the skin in Dressing Room > Classic Skins > Owned.\n"
        "\n"
        "Slim (Alex) arms: put the word slim anywhere in the file name.\n"
        "Custom / 4D skins: drop the model file next to the PNG with the same name,\n"
        "for example dragon.png plus dragon.geo.json (or dragon.json).\n"
        "The model identifier inside that file is used automatically.\n";
}

namespace SkinPack {
    std::filesystem::path getSkinsFolder() {
        return util::GetNecromancerPath() / "Skins";
    }

    std::filesystem::path findMojangDir() {
        std::error_code ec;

        wchar_t exeBuf[MAX_PATH] {};
        if (GetModuleFileNameW(nullptr, exeBuf, MAX_PATH) > 0) {
            std::filesystem::path exeDir = std::filesystem::path(exeBuf).parent_path();
            std::filesystem::path portable = exeDir / "games" / "com.mojang";
            if (std::filesystem::exists(portable, ec)) return portable;
        }

        wchar_t localBuf[MAX_PATH] {};
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", localBuf, MAX_PATH) > 0) {
            std::filesystem::path packages = std::filesystem::path(localBuf) / "Packages";
            if (std::filesystem::exists(packages, ec)) {
                for (auto const& entry : std::filesystem::directory_iterator(packages, ec)) {
                    if (ec) break;
                    std::wstring dirName = entry.path().filename().wstring();
                    if (dirName.rfind(L"Microsoft.Minecraft", 0) != 0) continue;
                    std::filesystem::path candidate = entry.path() / "LocalState" / "games" / "com.mojang";
                    if (std::filesystem::exists(candidate, ec)) return candidate;
                }
            }
        }

        return {};
    }

    void ensureFolder() {
        std::error_code ec;
        std::filesystem::path folder = getSkinsFolder();
        std::filesystem::create_directories(folder, ec);
        if (!std::filesystem::exists(folder / "readme.txt", ec)) {
            writeFile(folder / "readme.txt", readmeText);
        }
    }

    std::vector<SkinEntry> scan() {
        ensureFolder();

        std::vector<SkinEntry> entries;
        std::error_code ec;
        std::filesystem::path folder = getSkinsFolder();

        for (auto const& file : std::filesystem::directory_iterator(folder, ec)) {
            if (ec) break;
            if (!file.is_regular_file(ec)) continue;

            std::string name = file.path().filename().string();
            if (name.size() < 5 || toLower(name.substr(name.size() - 4)) != ".png") continue;

            auto size = file.file_size(ec);
            if (ec || size == 0 || size > 8ull * 1024ull * 1024ull) continue;

            SkinEntry entry {};
            entry.fileName = name;
            entry.fullPath = file.path();
            entry.fileSize = size;
            readPngSize(file.path(), entry.width, entry.height);

            std::string stem = name.substr(0, name.size() - 4);
            entry.localization = "necromancer." + sanitizeName(stem);
            entry.slim = toLower(stem).find("slim") != std::string::npos;
            entry.geometry = entry.slim ? "geometry.humanoid.customSlim" : "geometry.humanoid.custom";

            std::filesystem::path geoPath = findGeometryFile(folder, stem);
            if (!geoPath.empty()) {
                std::string text = readFile(geoPath);
                json doc = json::parse(text, nullptr, false);
                std::vector<std::string> ids = collectGeometryIds(doc);
                if (!ids.empty()) {
                    entry.geometry = ids.front();
                    entry.geometrySource = geoPath;
                    if (toLower(ids.front()).find("slim") != std::string::npos) entry.slim = true;
                }
            }

            entries.push_back(std::move(entry));
        }

        std::ranges::sort(entries, {}, &SkinEntry::fileName);
        return entries;
    }

    bool sync(std::string& report) {
        std::error_code ec;
        std::filesystem::path folder = getSkinsFolder();
        ensureFolder();

        std::filesystem::path mojang = findMojangDir();
        if (mojang.empty()) {
            report = "could not find the Minecraft data folder (looked next to the game exe and in "
                     "%LOCALAPPDATA%\\Packages\\Microsoft.Minecraft*)";
            return false;
        }

        std::vector<SkinEntry> entries = scan();
        if (entries.empty()) {
            report = "no PNG skins found in " + folder.string();
            return false;
        }

        std::string headerUuid;
        std::string moduleUuid;
        loadOrCreateUuids(folder, headerUuid, moduleUuid);

        std::filesystem::path packDir = mojang / "skin_packs" / "necromancer_skins";
        std::filesystem::remove_all(packDir, ec);
        std::filesystem::create_directories(packDir, ec);
        if (!std::filesystem::exists(packDir, ec)) {
            report = "failed to create the pack folder at " + packDir.string();
            return false;
        }

        std::string manifest = std::string() + "{\n  \"format_version\": 1,\n"
            "  \"header\": { \"name\": \"Necromancer Skins\", \"uuid\": \"" + headerUuid +
            "\", \"version\": [1, 0, 0] },\n"
            "  \"modules\": [ { \"type\": \"skin_pack\", \"uuid\": \"" + moduleUuid +
            "\", \"version\": [1, 0, 0] } ]\n}\n";
        if (!writeFile(packDir / "manifest.json", manifest)) {
            report = "failed to write manifest.json";
            return false;
        }

        json mergedModern = json::array();
        json mergedLegacy = json::object();
        std::string modernFormat;
        std::string legacyFormat;
        size_t customModels = 0;
        size_t copyFailures = 0;

        std::string skinsJson = "{\n  \"skins\": [\n";
        std::string lang = std::string("skinpack.necromancer_skins.name=") + packHeaderName +
                           "\nskinpack.necromancer_skins=necromancer_skins\n";

        for (size_t i = 0; i < entries.size(); i++) {
            SkinEntry const& entry = entries[i];
            std::string locName = "skin" + std::to_string(i);

            skinsJson += std::string() + "    { \"localization_name\": \"" + locName +
                "\", \"geometry\": \"" + entry.geometry + "\", \"texture\": \"" + entry.fileName +
                "\", \"type\": \"free\" }" + (i + 1 < entries.size() ? "," : "") + "\n";

            std::string label = entry.fileName.substr(0, entry.fileName.size() - 4);
            lang += std::string("skin.") + packHeaderName + "." + locName + "=" + label + "\n";
            lang += "skin.necromancer_skins." + locName + "=" + label + "\n";

            std::error_code copyEc;
            std::filesystem::copy_file(entry.fullPath, packDir / entry.fileName,
                                       std::filesystem::copy_options::overwrite_existing, copyEc);
            if (copyEc) copyFailures++;

            if (entry.geometrySource.empty()) continue;

            json doc = json::parse(readFile(entry.geometrySource), nullptr, false);
            if (!doc.is_object()) continue;

            if (auto modern = doc.find("minecraft:geometry"); modern != doc.end() && modern->is_array()) {
                for (auto const& model : *modern) mergedModern.push_back(model);
                if (auto fv = doc.find("format_version"); fv != doc.end() && fv->is_string()) {
                    modernFormat = fv->get<std::string>();
                }
            } else {
                for (auto const& [key, value] : doc.items()) {
                    if (key == "format_version") {
                        if (value.is_string()) legacyFormat = value.get<std::string>();
                        continue;
                    }
                    mergedLegacy[key] = value;
                }
            }
            customModels++;
        }

        skinsJson += "  ]\n}\n";
        if (!writeFile(packDir / "skins.json", skinsJson)) {
            report = "failed to write skins.json";
            return false;
        }

        writeFile(packDir / "texts" / "en_US.lang", lang);
        writeFile(packDir / "texts" / "languages.json", "[ \"en_US\" ]\n");

        std::error_code iconEc;
        std::filesystem::copy_file(entries.front().fullPath, packDir / "pack_icon.png",
                                   std::filesystem::copy_options::overwrite_existing, iconEc);

        registerKnownPack(mojang, packDir, headerUuid);

        if (!mergedModern.empty()) {
            json out = json::object();
            out["format_version"] = modernFormat.empty() ? std::string("1.12.0") : modernFormat;
            out["minecraft:geometry"] = mergedModern;
            writeFile(packDir / "geometry.json", out.dump(2) + "\n");
        } else if (!mergedLegacy.empty()) {
            json out = json::object();
            out["format_version"] = legacyFormat.empty() ? std::string("1.8.0") : legacyFormat;
            for (auto const& [key, value] : mergedLegacy.items()) out[key] = value;
            writeFile(packDir / "geometry.json", out.dump(2) + "\n");
        }

        report = "synced " + std::to_string(entries.size()) + " skin(s)";
        if (customModels > 0) report += " (" + std::to_string(customModels) + " with a custom model)";
        if (copyFailures > 0) report += ", " + std::to_string(copyFailures) + " file(s) failed to copy";
        report += " - restart Minecraft once, then pick it in the dressing room";

        Logger::Info("[Skins] {} -> {}", report, packDir.string());
        return true;
    }

    void openFolder() {
        ensureFolder();
        ShellExecuteW(nullptr, L"open", getSkinsFolder().wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}
