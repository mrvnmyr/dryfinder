#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

// ----------------------------- Debug ---------------------------------

static bool g_debug = false;

static void dlog(const std::string& msg) {
    if (g_debug) std::cerr << "[debug] " << msg << '\n';
}

// ----------------------------- Utilities ------------------------------

static inline std::string to_generic_string(const fs::path& p) {
    return p.generic_string();
}

static inline bool has_glob_chars(const std::string& s) {
    for (char c : s) if (c == '*' || c == '?') return true;
    return false;
}

static std::string lstrip_dots_slashes(std::string s) {
    // normalize leading "./"
    while (s.size() >= 2 && s[0] == '.' && (s[1] == '/' )) {
        s.erase(0, 2);
    }
    // and leading '/' (avoid absolute)
    while (!s.empty() && s[0] == '/') s.erase(0, 1);
    return s;
}

static std::string strip_utf8_bom(const std::string& s) {
    if (s.size() >= 3 &&
        static_cast<unsigned char>(s[0]) == 0xEF &&
        static_cast<unsigned char>(s[1]) == 0xBB &&
        static_cast<unsigned char>(s[2]) == 0xBF) {
        return s.substr(3);
    }
    return s;
}

static void rstrip_cr(std::string& s) {
    if (!s.empty() && s.back() == '\r') s.pop_back();
}

// Ignore indentation helper: strip leading spaces/tabs
static inline std::string strip_indent(const std::string& in) {
    size_t i = 0;
    while (i < in.size() && (in[i] == ' ' || in[i] == '\t')) ++i;
    return in.substr(i);
}

// YAML escaping for double-quoted scalars
static std::string yaml_escape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    out.push_back('"');
    for (char c : in) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '\"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    // control chars -> \xHH
                    char buf[5];
                    std::snprintf(buf, sizeof(buf), "\\x%02X", (unsigned char)c);
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
    return out;
}

// ----------------------------- Glob Engine ----------------------------
// Minimal glob-to-regex supporting *, ?, ** (recursive)
// We determine a base directory (prefix before first glob char),
// iterate recursively from there, and match the *suffix pattern* as regex
// against the path relative to that base.

struct CompiledPattern {
    fs::path base_dir;
    std::regex regex_suffix; // matches relative path from base_dir
};

static fs::path compute_base_dir(const std::string& generic_pattern) {
    // Find first glob char
    size_t first = std::string::npos;
    for (size_t i = 0; i < generic_pattern.size(); ++i) {
        char c = generic_pattern[i];
        if (c == '*' || c == '?') { first = i; break; }
    }
    std::string prefix = generic_pattern;
    if (first != std::string::npos) {
        // take substring up to last '/' before first glob
        size_t slash = generic_pattern.rfind('/', first);
        if (slash != std::string::npos) prefix = generic_pattern.substr(0, slash);
        else prefix = ".";
    }
    if (prefix.empty()) prefix = ".";
    return fs::path(prefix);
}

static std::string to_regex_from_glob_suffix(const std::string& suffix) {
    // Convert glob suffix (relative to base) to a full regex ^...$
    std::string out;
    out.reserve(suffix.size() * 2 + 5);
    out += "^";
    for (size_t i = 0; i < suffix.size(); ++i) {
        char c = suffix[i];
        if (c == '*') {
            // check for **
            size_t j = i;
            while (j < suffix.size() && suffix[j] == '*') ++j;
            size_t stars = j - i;
            i = j - 1;
            if (stars >= 2) {
                out += ".*"; // ** -> match across separators
            } else {
                out += "[^/]*"; // * -> no slash
            }
        } else if (c == '?') {
            out += "[^/]";
        } else {
            // escape regex specials
            switch (c) {
                case '.': case '+': case '(': case ')':
                case '^': case '$': case '|': case '{':
                case '}': case '[': case ']': case '\\':
                    out.push_back('\\'); out.push_back(c); break;
                default:
                    out.push_back(c);
            }
        }
    }
    out += "$";
    return out;
}

static CompiledPattern compile_pattern(const std::string& pattern_raw) {
    std::string p = to_generic_string(fs::path(pattern_raw));
    p = lstrip_dots_slashes(p);
    fs::path base = compute_base_dir(p);
    std::string base_generic = to_generic_string(base);
    base_generic = lstrip_dots_slashes(base_generic);

    // Build suffix: pattern without the base prefix (and possible '/')
    std::string suffix;
    if (!base_generic.empty()) {
        if (p.rfind(base_generic + "/", 0) == 0) {
            suffix = p.substr(base_generic.size() + 1);
        } else if (p == base_generic) {
            suffix = ""; // degenerate, match directory itself
        } else {
            // Base couldn't be trimmed as prefix (e.g., relative forms). Fallback:
            size_t pos = p.find_first_of("*?");
            size_t slash = p.rfind('/', (pos == std::string::npos ? p.size()-1 : pos));
            if (slash != std::string::npos && slash + 1 < p.size())
                suffix = p.substr(slash + 1);
            else
                suffix = p;
        }
    } else {
        suffix = p;
    }
    if (suffix.empty()) suffix = "**"; // match everything under base

    std::string re_str = to_regex_from_glob_suffix(suffix);
    std::regex re(re_str, std::regex::ECMAScript);
    return { base, re };
}

static std::vector<fs::path> expand_globs(const std::vector<std::string>& patterns) {
    std::vector<fs::path> results;
    std::unordered_set<std::string> seen; // generic string paths to dedupe
    for (const auto& pat : patterns) {
        CompiledPattern cp = compile_pattern(pat);
        fs::path base = cp.base_dir.empty() ? fs::path(".") : cp.base_dir;
        dlog(std::string("glob pattern: ") + pat +
             " | base=" + to_generic_string(base));
        if (!fs::exists(base)) {
            dlog("  base does not exist, skipping");
            continue;
        }
        if (fs::is_regular_file(base)) {
            std::string rel = to_generic_string(base.filename());
            if (std::regex_match(rel, cp.regex_suffix)) {
                std::string g = to_generic_string(base);
                if (seen.insert(g).second) results.push_back(base);
            }
            continue;
        }

        size_t added = 0;
        for (fs::recursive_directory_iterator it(base, fs::directory_options::follow_directory_symlink);
             it != fs::recursive_directory_iterator(); ++it) {
            if (!it->is_regular_file()) continue;
            std::error_code ec;
            fs::path relPath = fs::relative(it->path(), base, ec);
            if (ec) relPath = it->path().lexically_relative(base); // best-effort
            std::string rel = to_generic_string(relPath);
            if (std::regex_match(rel, cp.regex_suffix)) {
                std::string g = to_generic_string(it->path());
                if (seen.insert(g).second) {
                    results.push_back(it->path());
                    ++added;
                }
            }
        }
        dlog("  matched files: " + std::to_string(added));
    }
    return results;
}

// --------------------------- Duplicate Finder -------------------------

struct FileData {
    fs::path path;
    std::vector<std::string> lines; // normalized LF, no trailing CR
};

struct Hit {
    std::string path;   // generic string
    size_t start_line;  // 1-based inclusive
    size_t end_line;    // 1-based inclusive
};

struct DuplicateBlock {
    std::vector<std::string> lines; // the block lines (from first occurrence)
    std::vector<Hit> hits;
};

static std::vector<std::string> read_lines_normalized(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::vector<std::string> out;
    if (!in) return out;
    std::string line;
    bool first = true;
    size_t count = 0;
    while (std::getline(in, line)) {
        rstrip_cr(line);
        if (first) {
            line = strip_utf8_bom(line);
            first = false;
        }
        out.push_back(line);
        ++count;
    }
    dlog("read " + to_generic_string(p) + " (" + std::to_string(count) + " lines)");
    return out;
}

// Join lines [i, j) with '\n'
static std::string join_lines(const std::vector<std::string>& v, size_t i, size_t j) {
    std::ostringstream oss;
    for (size_t k = i; k < j; ++k) {
        if (k > i) oss << '\n';
        oss << v[k];
    }
    return oss.str();
}

// Join with optional indentation stripping (for keys)
static std::string join_lines_norm(const std::vector<std::string>& v, size_t i, size_t j, bool ignore_indent) {
    std::ostringstream oss;
    for (size_t k = i; k < j; ++k) {
        if (k > i) oss << '\n';
        if (ignore_indent) oss << strip_indent(v[k]);
        else oss << v[k];
    }
    return oss.str();
}

struct Occurrence {
    int file_index;
    size_t start; // 0-based line index
};

static DuplicateBlock build_maximal_block(const std::vector<FileData>& files,
                                          const std::vector<Occurrence>& occs_in,
                                          size_t seed_len,
                                          bool ignore_indent) {
    // Work on a local copy as we adjust start when extending backward.
    std::vector<Occurrence> occs = occs_in;

    auto equal_line = [ignore_indent](const std::string& a, const std::string& b) {
        if (!ignore_indent) return a == b;
        return strip_indent(a) == strip_indent(b);
    };

    // Extend backward as long as all occurrences have same previous line
    bool can = true;
    while (can) {
        for (const auto& oc : occs) {
            if (oc.start == 0) { can = false; break; }
        }
        if (!can) break;
        const std::string& ref = files[occs[0].file_index].lines[occs[0].start - 1];
        for (size_t i = 1; i < occs.size(); ++i) {
            const auto& fd = files[occs[i].file_index];
            if (!equal_line(fd.lines[occs[i].start - 1], ref)) { can = false; break; }
        }
        if (can) {
            for (auto& oc : occs) oc.start -= 1;
        }
    }

    // Extend forward
    size_t length = seed_len;
    while (true) {
        size_t next_idx0 = occs[0].start + length;
        const auto& f0 = files[occs[0].file_index];
        if (next_idx0 >= f0.lines.size()) break;
        const std::string& ref = f0.lines[next_idx0];
        bool all_ok = true;
        for (size_t i = 1; i < occs.size(); ++i) {
            size_t next_idx = occs[i].start + length;
            const auto& fi = files[occs[i].file_index];
            if (next_idx >= fi.lines.size() || !equal_line(fi.lines[next_idx], ref)) {
                all_ok = false; break;
            }
        }
        if (!all_ok) break;
        length += 1;
    }

    // Build block using lines from the first file's occurrence (original indentation)
    DuplicateBlock block;
    block.lines = std::vector<std::string>(
        files[occs[0].file_index].lines.begin() + static_cast<std::ptrdiff_t>(occs[0].start),
        files[occs[0].file_index].lines.begin() + static_cast<std::ptrdiff_t>(occs[0].start + length)
    );
    block.hits.reserve(occs.size());
    for (const auto& oc : occs) {
        Hit h;
        h.path = to_generic_string(files[oc.file_index].path);
        h.start_line = oc.start + 1;               // 1-based
        h.end_line   = oc.start + length;          // 1-based inclusive
        block.hits.push_back(std::move(h));
    }
    return block;
}

static std::vector<DuplicateBlock>
find_repeated_blocks(const std::vector<fs::path>& files_paths, size_t min_lines, bool ignore_indent) {
    // Load all files
    std::vector<FileData> files;
    files.reserve(files_paths.size());
    for (const auto& p : files_paths) {
        FileData fd;
        fd.path = p;
        fd.lines = read_lines_normalized(p);
        files.push_back(std::move(fd));
    }

    dlog("total files loaded: " + std::to_string(files.size()));

    // Map seed-string -> occurrences
    // seed-string is the concatenation of min_lines lines with '\n'
    std::unordered_map<std::string, std::vector<Occurrence>> seeds;
    for (int idx = 0; idx < static_cast<int>(files.size()); ++idx) {
        const auto& f = files[idx];
        if (f.lines.size() < min_lines) continue;
        for (size_t i = 0; i + min_lines <= f.lines.size(); ++i) {
            std::string key = join_lines_norm(f.lines, i, i + min_lines, ignore_indent);
            seeds[key].push_back({ idx, i });
        }
    }

    size_t candidate_seeds = 0;
    for (const auto& kv : seeds) if (kv.second.size() >= 2) ++candidate_seeds;
    dlog("seed windows: " + std::to_string(seeds.size()) +
         " | candidate seeds (>=2 hits): " + std::to_string(candidate_seeds));

    // Aggregate maximal blocks keyed by (possibly normalized) content to dedupe/merge hits
    struct Agg {
        std::vector<std::string> lines;
        std::unordered_set<std::string> hit_keys;
        std::vector<Hit> hits;
    };
    std::unordered_map<std::string, Agg> by_content; // content key -> Agg

    size_t groups_built = 0;
    for (auto& [seed, occs] : seeds) {
        if (occs.size() < 2) continue;
        DuplicateBlock block = build_maximal_block(files, occs, min_lines, ignore_indent);
        // Use normalized content as key if ignoring indentation
        std::string content_key = join_lines_norm(block.lines, 0, block.lines.size(), ignore_indent);
        auto& agg = by_content[content_key];
        if (agg.lines.empty()) agg.lines = block.lines;

        for (const auto& h : block.hits) {
            std::ostringstream key;
            key << h.path << '\n' << h.start_line << '\n' << h.end_line;
            std::string k = key.str();
            if (agg.hit_keys.insert(k).second) {
                agg.hits.push_back(h);
            }
        }
        ++groups_built;
    }

    dlog("maximal groups built: " + std::to_string(groups_built));

    // Build final list, only those with >= 2 unique hits
    std::vector<DuplicateBlock> out;
    out.reserve(by_content.size());
    for (auto& [_, agg] : by_content) {
        if (agg.hits.size() >= 2) {
            DuplicateBlock b;
            b.lines = std::move(agg.lines);
            b.hits  = std::move(agg.hits);
            out.push_back(std::move(b));
        }
    }
    dlog("final duplicate blocks: " + std::to_string(out.size()));
    return out;
}

// --------------------------- YAML Emission ----------------------------

static size_t bytes_of_lines(const std::vector<std::string>& lines) {
    size_t n = 0;
    if (lines.empty()) return 0;
    for (const auto& s : lines) n += s.size() + 1; // + '\n'
    return n;
}

static void print_yaml(const std::vector<DuplicateBlock>& blocks) {
    std::cout << "blocks:\n";
    for (const auto& b : blocks) {
        const size_t line_count = b.lines.size();
        const size_t byte_count = bytes_of_lines(b.lines);
        std::cout << "  - lines: " << line_count << "\n";
        std::cout << "    bytes: " << byte_count << "\n";
        std::cout << "    occurrences: " << b.hits.size() << "\n";
        std::cout << "    hits:\n";
        // Stable order by file, then start_line
        std::vector<Hit> hits = b.hits;
        std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b){
            if (a.path != b.path) return a.path < b.path;
            if (a.start_line != b.start_line) return a.start_line < b.start_line;
            return a.end_line < b.end_line;
        });
        for (const auto& h : hits) {
            std::cout << "      - file: " << yaml_escape(h.path) << "\n";
            std::cout << "        start_line: " << h.start_line << "\n";
            std::cout << "        end_line: " << h.end_line << "\n";
        }
        std::cout << "    content: |\n";
        for (const auto& line : b.lines) {
            std::cout << "      " << line << "\n";
        }
    }
}

// ------------------------------- Main --------------------------------

static void print_usage_and_exit(const char* argv0) {
    std::cerr << "Usage: " << argv0 << " [--debug] [--ignore-indentation] "
              << "--min-lines N <glob> [<glob>...]\n";
    std::cerr << "Example: " << argv0 << " --ignore-indentation --min-lines 9 "
              << "\"./foo/**/*.cpp\" \"*.c\"\n";
    std::exit(2);
}

int main(int argc, char** argv) {
    if (argc < 3) {
        print_usage_and_exit(argv[0]);
    }
    size_t min_lines = 0;
    bool ignore_indent = false;
    std::vector<std::string> patterns;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--min-lines") {
            if (i + 1 >= argc) {
                std::cerr << "--min-lines requires a value\n";
                return 2;
            }
            try {
                long v = std::stol(argv[++i]);
                if (v < 1) throw std::invalid_argument("min-lines < 1");
                min_lines = static_cast<size_t>(v);
            } catch (...) {
                std::cerr << "Invalid --min-lines value\n";
                return 2;
            }
        } else if (arg == "--debug") {
            g_debug = true;
        } else if (arg == "--ignore-indentation") {
            ignore_indent = true;
        } else {
            patterns.push_back(arg);
        }
    }

    if (min_lines == 0 || patterns.empty()) {
        print_usage_and_exit(argv[0]);
    }

    dlog("min_lines=" + std::to_string(min_lines));
    dlog(std::string("ignore_indentation=") + (ignore_indent ? "true" : "false"));
    {
        std::ostringstream oss;
        oss << "patterns:";
        for (const auto& p : patterns) oss << " " << p;
        dlog(oss.str());
    }

    // Expand globs to files
    std::vector<fs::path> files = expand_globs(patterns);
    std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b){
        return to_generic_string(a) < to_generic_string(b);
    });
    dlog("files matched: " + std::to_string(files.size()));
    for (size_t i = 0; g_debug && i < files.size() && i < 5; ++i) {
        dlog("  file[" + std::to_string(i) + "]: " + to_generic_string(files[i]));
    }

    // Find duplicates
    std::vector<DuplicateBlock> blocks = find_repeated_blocks(files, min_lines, ignore_indent);

    // Sort by size/length (lines desc), then by occurrences desc, then by content
    std::sort(blocks.begin(), blocks.end(), [](const DuplicateBlock& a, const DuplicateBlock& b){
        if (a.lines.size() != b.lines.size()) return a.lines.size() > b.lines.size();
        if (a.hits.size()  != b.hits.size())  return a.hits.size()  > b.hits.size();
        if (!a.lines.empty() && !b.lines.empty() && a.lines[0] != b.lines[0])
            return a.lines[0] < b.lines[0];
        return a.lines < b.lines;
    });

    dlog("blocks after sort: " + std::to_string(blocks.size()));
    print_yaml(blocks);
    return 0;
}
