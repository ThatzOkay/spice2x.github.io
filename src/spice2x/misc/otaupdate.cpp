#include "otaupdate.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include <external/hash-library/crc32.h>
#include <external/hash-library/md5.h>

#include "util/logging.h"

namespace fs = std::filesystem;

namespace ota {

    /*
     * update.qsv is a CSV manifest, one file per line:
     *   0: unused, 1: path, 2-6: unused, 7: size, 8: unused, 9: checksum, 10: checksum type
     * The path looks like /./<model>/contents/<path relative to the game directory>. The checksum
     * is written byte by byte, so a CRC32 of 0x8845E097 is stored as 97E04588. MD5 is plain hex.
     */
    struct Entry {
        fs::path source;
        fs::path relative;
        uintmax_t size = 0;
        std::string checksum;
        std::string type;
    };

    static std::vector<std::string> split(const std::string &text, char delimiter) {
        std::vector<std::string> parts;
        std::stringstream stream(text);
        std::string part;

        while (std::getline(stream, part, delimiter))
            parts.push_back(part);

        return parts;
    }

    static bool equals_nocase(const std::string &a, const std::string &b) {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
            return std::toupper(static_cast<unsigned char>(x)) == std::toupper(static_cast<unsigned char>(y));
        });
    }

    // anything that could leave the game directory is rejected
    static bool parse_path(std::string path, Entry &entry) {
        std::replace(path.begin(), path.end(), '\\', '/');

        if (path.rfind("/./", 0) != 0)
            return false;
        path.erase(0, 3);
        entry.source = fs::path(path).lexically_normal();

        // skip the model directory
        const auto model_end = path.find('/');
        if (model_end == std::string::npos)
            return false;
        path.erase(0, model_end + 1);

        if (path.rfind("contents/", 0) != 0)
            return false;
        path.erase(0, 9);

        if (path.empty() || path.find(':') != std::string::npos)
            return false;

        entry.relative = fs::path(path).lexically_normal();
        if (entry.relative.empty() || entry.relative.is_absolute()
                || entry.relative.has_root_name() || entry.relative.has_root_directory()) {
            return false;
        }
        for (const auto &part : entry.relative) {
            if (part == "..")
                return false;
        }

        return true;
    }

    static bool parse_manifest(const std::string &text, std::vector<Entry> &entries) {
        std::stringstream stream(text);
        std::string line;

        while (std::getline(stream, line)) {

            // lines end with CRLF and the manifest itself ends with a NUL
            line.erase(std::remove_if(line.begin(), line.end(), [](char c) {
                return c == '\r' || c == '\0';
            }), line.end());
            if (line.empty())
                continue;

            const auto fields = split(line, ',');
            if (fields.size() < 11) {
                log_warning("ota", "malformed manifest line: {}", line);
                return false;
            }

            Entry entry;
            if (!parse_path(fields[1], entry)) {
                log_warning("ota", "unsupported path in manifest: {}", fields[1]);
                return false;
            }
            try {
                entry.size = std::stoull(fields[7]);
            } catch (const std::exception &) {
                log_warning("ota", "invalid size for {}: {}", fields[1], fields[7]);
                return false;
            }
            entry.checksum = fields[9];
            entry.type = fields[10];
            entries.push_back(std::move(entry));
        }

        return true;
    }

    // returns the checksum in the notation of the manifest, empty if the type is not supported
    static std::string compute_checksum(const fs::path &file, const std::string &type) {
        const bool is_crc32 = equals_nocase(type, "CRC32");
        const bool is_md5 = equals_nocase(type, "MD5");
        if (!is_crc32 && !is_md5)
            return "";

        std::ifstream stream(file, std::ios::binary);
        if (!stream)
            return "";

        CRC32 crc32;
        MD5 md5;
        std::vector<char> buffer(1024 * 1024);
        while (stream.read(buffer.data(), buffer.size()) || stream.gcount() > 0) {
            if (is_crc32)
                crc32.add(buffer.data(), static_cast<size_t>(stream.gcount()));
            else
                md5.add(buffer.data(), static_cast<size_t>(stream.gcount()));
        }

        if (is_md5)
            return md5.getHash();

        // hash-library returns the CRC most significant byte first
        unsigned char digest[CRC32::HashBytes];
        crc32.getHash(digest);
        std::string result;
        for (int i = CRC32::HashBytes - 1; i >= 0; i--) {
            static const char *hex = "0123456789ABCDEF";
            result += hex[digest[i] >> 4];
            result += hex[digest[i] & 0xF];
        }
        return result;
    }

    // logs every 10% of progress
    static void log_progress(const char *action, size_t done, size_t total, size_t &last_percent) {
        const size_t percent = done * 100 / total;
        if (percent / 10 != last_percent / 10)
            log_info("ota", "{}: {}%", action, percent);
        last_percent = percent;
    }

    static bool verify(const fs::path &ex_dir, const std::vector<Entry> &entries) {
        std::error_code ec;
        size_t last_percent = 0;

        for (size_t i = 0; i < entries.size(); i++) {
            const auto &entry = entries[i];
            const auto file = ex_dir / entry.source;

            if (!fs::is_regular_file(file, ec) || fs::file_size(file, ec) != entry.size) {
                log_warning("ota", "missing or wrong size: {}", entry.source.string());
                return false;
            }

            const auto checksum = compute_checksum(file, entry.type);
            if (checksum.empty()) {
                log_warning("ota", "unsupported checksum type {} for {}", entry.type, entry.source.string());
                return false;
            }
            if (!equals_nocase(checksum, entry.checksum)) {
                log_warning("ota", "checksum mismatch for {}: expected {}, got {}",
                        entry.source.string(), entry.checksum, checksum);
                return false;
            }

            log_progress("verifying", i + 1, entries.size(), last_percent);
        }

        return true;
    }

    static std::string timestamp() {
        char buffer[32];
        const auto now = std::time(nullptr);
        std::strftime(buffer, sizeof(buffer), "%Y%m%d-%H%M%S", std::localtime(&now));
        return buffer;
    }

    static bool read_text(const fs::path &path, std::string &text) {
        std::ifstream stream(path, std::ios::binary);
        if (!stream)
            return false;

        text.assign((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        return true;
    }

    // the text between open and the next '<', npos if open is not found
    static size_t find_value(const std::string &text, const std::string &open, size_t &length) {
        const auto begin = text.find(open);
        if (begin == std::string::npos)
            return begin;

        const auto value = begin + open.size();
        length = text.find('<', value) - value;
        return value;
    }

    /*
     * The e-amusement config carries the datecode of the installed version. An update replaces
     * prop/bootstrap.xml, whose release_code is that datecode, so bring the config files along.
     */
    static void sync_datecode(const fs::path &game_dir, const fs::path &backup_dir) {
        std::string bootstrap;
        if (!read_text(game_dir / "prop" / "bootstrap.xml", bootstrap))
            return;

        size_t length = 0;
        const auto release_begin = find_value(bootstrap, "<release_code>", length);
        if (release_begin == std::string::npos)
            return;
        const auto release_code = bootstrap.substr(release_begin, length);
        if (release_code.size() != 10)
            return;

        for (const auto name : {"ea3-config.xml", "ea3-ident.xml"}) {
            const auto path = game_dir / "prop" / name;
            std::string text;
            if (!read_text(path, text))
                continue;

            const auto begin = find_value(text, "<ext __type=\"str\">", length);
            if (begin == std::string::npos)
                continue;

            // datecodes have a fixed length, so a string comparison orders them
            const auto current = text.substr(begin, length);
            if (current.size() != release_code.size() || current >= release_code)
                continue;

            std::error_code ec;
            fs::create_directories(backup_dir / "prop", ec);
            fs::copy_file(path, backup_dir / "prop" / name, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                log_warning("ota", "failed to back up {}: {}", name, ec.message());
                continue;
            }

            text.replace(begin, length, release_code);
            std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
            log_info("ota", "datecode in {}: {} -> {}", name, current, release_code);
        }
    }

    static bool install(
            const fs::path &ex_dir,
            const fs::path &game_dir,
            const fs::path &backup_dir,
            const std::vector<Entry> &entries)
    {
        std::error_code ec;
        size_t last_percent = 0;

        struct Installed {
            fs::path relative;
            bool backed_up;
        };
        std::vector<Installed> installed;

        // put everything back the way it was
        const auto rollback = [&]() {
            log_warning("ota", "rolling back");
            for (auto it = installed.rbegin(); it != installed.rend(); ++it) {
                if (it->backed_up) {
                    fs::copy_file(backup_dir / it->relative, game_dir / it->relative,
                            fs::copy_options::overwrite_existing, ec);
                } else {
                    fs::remove(game_dir / it->relative, ec);
                }
            }
        };

        for (size_t i = 0; i < entries.size(); i++) {
            const auto &entry = entries[i];
            const auto source = ex_dir / entry.source;
            const auto destination = game_dir / entry.relative;
            const bool exists = fs::exists(destination, ec);

            // keep the old file
            if (exists) {
                fs::create_directories((backup_dir / entry.relative).parent_path(), ec);
                fs::copy_file(destination, backup_dir / entry.relative, fs::copy_options::overwrite_existing, ec);
                if (ec) {
                    log_warning("ota", "failed to back up {}: {}", entry.relative.string(), ec.message());
                    rollback();
                    return false;
                }
            }

            // write next to the destination first so a failed copy never leaves a broken file behind
            fs::create_directories(destination.parent_path(), ec);
            auto temp = destination;
            temp += ".ota-tmp";
            fs::copy_file(source, temp, fs::copy_options::overwrite_existing, ec);
            if (!ec)
                fs::rename(temp, destination, ec);
            if (ec) {
                log_warning("ota", "failed to install {}: {}", entry.relative.string(), ec.message());
                fs::remove(temp, ec);
                rollback();
                return false;
            }

            installed.push_back({entry.relative, exists});
            log_progress("installing", i + 1, entries.size(), last_percent);
        }

        return true;
    }

    ApplyResult apply_staged_updates(const fs::path &game_dir) {
        ApplyResult result;
        std::error_code ec;

        const auto drive_f = game_dir / "dev" / "vfs" / "drive_f";
        if (!fs::is_directory(drive_f, ec))
            return result;

        const auto backup_dir = game_dir / "dev" / "vfs" / "drive_d" / "update-backup" / timestamp();

        for (const auto &model_dir : fs::directory_iterator(drive_f, ec)) {
            const auto ex_dir = model_dir.path() / "update" / "ex";
            const auto manifest_path = ex_dir / "update.qsv";
            if (!fs::is_regular_file(manifest_path, ec))
                continue;

            log_info("ota", "found staged update: {}", ex_dir.string());

            std::string text;
            std::vector<Entry> entries;
            if (!read_text(manifest_path, text) || !parse_manifest(text, entries) || entries.empty()
                    || !verify(ex_dir, entries) || !install(ex_dir, game_dir, backup_dir, entries)) {
                log_warning("ota", "update was not applied, the staged files are left in place");
                result.status = ApplyStatus::Failed;
                return result;
            }

            sync_datecode(game_dir, backup_dir);

            // keep a record of what was applied, like select.exe does
            const auto record = game_dir / "dev" / "vfs" / "drive_d" / "update.qsv";
            fs::create_directories(record.parent_path(), ec);
            fs::copy_file(manifest_path, record, fs::copy_options::overwrite_existing, ec);

            fs::remove_all(ex_dir, ec);

            log_info("ota", "applied update, {} files", entries.size());
            result.status = ApplyStatus::Applied;
            result.files += entries.size();
        }

        return result;
    }
}
