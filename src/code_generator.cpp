#include "code_generator.h"
#include "module_parser.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <regex>
#include <set>
#include <map>
#include <dirent.h>
#include <sys/stat.h>
#include <cstdlib>
#include <limits.h>

CodeGenerator::CodeGenerator(const std::string& modules_dir)
  :modules_dir_(modules_dir)
  ,conn_builder_(nullptr)
  ,total_connections_(0)
  ,vlwide_ports_(0)
  ,top_outputs_(0) {
}

CodeGenerator::CodeGenerator(const std::string& modules_dir,
                             SimulatorFactory::SimulatorType simulator_type)
  : CodeGenerator(modules_dir) {
  // Interpret this legacy constructor as "force a single backend".
  if (simulator_type == SimulatorFactory::SimulatorType::VERILATOR) {
    backend_policy_ = BackendPolicy::VERILATOR_ONLY;
  } else if (simulator_type == SimulatorFactory::SimulatorType::GSIM) {
    backend_policy_ = BackendPolicy::GSIM_ONLY;
  } else {
    backend_policy_ = BackendPolicy::MIXED;
  }
}

static std::string path_dirname(const std::string& p) {
  size_t pos = p.find_last_of('/');
  if (pos == std::string::npos) return "./";
  return p.substr(0, pos);
}

static std::string path_basename(const std::string& p) {
  size_t pos = p.find_last_of('/');
  return (pos == std::string::npos) ? p : p.substr(pos + 1);
}

static bool file_exists(const std::string& path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0;
}

static std::string to_abs_path(const std::string& p) {
  char buf[PATH_MAX];
  if (realpath(p.c_str(), buf)) return std::string(buf);
  return p;
}

static const PortInfo* find_port_by_logical_name(const ModuleInfo* m, const std::string& logical_name) {
  if (!m) return nullptr;
  for (const auto& p : m->ports) {
    if (p.name == logical_name) return &p;
  }
  return nullptr;
}

static int words_for_width(int w) {
  return (w + 31) / 32;
}

// Emit a large propagate function as multiple smaller helper functions to avoid
// extremely large single functions that can stress compilers (and slow builds).
std::string CodeGenerator::generate_chunked_propagate(const std::vector<PortConnection>& connections,
                                                      const std::string& function_name,
                                                      const std::string& comment,
                                                      size_t chunk_size) {
  std::ostringstream oss;
  const size_t n = connections.size();
  const size_t chunks = chunk_size == 0 ? 1 : (n + chunk_size - 1) / chunk_size;

  oss << "void VCorvusTopWrapper::" << function_name << "() {\n";
  oss << "    // " << comment << "\n";
  oss << "    // Auto-generated: " << n << " connections\n";
  if (n == 0) {
    oss << "    // No connections\n";
  } else if (chunks == 1) {
    for (const auto& c : connections) {
      oss << generate_assignment(c);
    }
  } else {
    for (size_t i = 0; i < chunks; ++i) {
      oss << "    " << function_name << "_chunk" << i << "();\n";
    }
  }
  oss << "}\n\n";

  if (n == 0 || chunks <= 1) return oss.str();

  for (size_t i = 0; i < chunks; ++i) {
    const size_t begin = i * chunk_size;
    const size_t end = std::min(n, begin + chunk_size);
    oss << "void VCorvusTopWrapper::" << function_name << "_chunk" << i << "() {\n";
    oss << "    // chunk " << i << " [" << begin << ", " << end << ")\n";
    for (size_t j = begin; j < end; ++j) {
      oss << generate_assignment(connections[j]);
    }
    oss << "}\n\n";
  }

  return oss.str();
}

bool CodeGenerator::load_data() {
  std::cout << "\n=== Loading Module Data ===" << std::endl;

  std::vector<ModuleDiscoveryResult> discovery_results;

  // Use ModuleDiscoveryManager to discover modules from all simulators
  ModuleDiscoveryManager manager;
  discovery_results = manager.discover_all_modules(modules_dir_);
  manager.print_discovery_statistics();

  if (discovery_results.empty()) {
    std::cerr << "No modules found in directory: " << modules_dir_ << std::endl;
    return false;
  }

  // Parse each discovered module candidate using the appropriate parser.
  // Then select one backend per logical module name (e.g. prefer GSIM for COMB, Verilator for SEQ).
  std::map<std::string, std::vector<ModuleInfo>> candidates;
  for (const auto& result : discovery_results) {
    std::cout << "  Parsing " << result.header_path << " [" << result.simulator_name << "]..." << std::endl;
    
    std::unique_ptr<ModuleParser> parser = ModuleParserFactory::create(result.simulator_name);
    if (!parser) {
      std::cerr << "Failed to create parser for simulator: " << result.simulator_name << std::endl;
      return false;
    }
    
    ModuleInfo info = parser->parse(result.header_path);
    if (info.simulator_name.empty()) info.simulator_name = result.simulator_name;

    if (info.ports.empty()) {
      std::cerr << "Failed to parse module: " << result.module_name << std::endl;
      return false;
    }

    candidates[info.module_name].push_back(info);
    std::cout << "    -> " << info.ports.size() << " ports" << std::endl;
  }

  auto score_candidate = [&](const ModuleInfo& m) -> int {
    // Policy is configurable:
    // - MIXED (default): COMB prefers GSIM; SEQ/EXTERNAL prefer Verilator.
    // - VERILATOR_ONLY: always prefer Verilator when available.
    // - GSIM_ONLY: always prefer GSIM when available.
    switch (backend_policy_) {
      case BackendPolicy::VERILATOR_ONLY:
        if (m.is_verilator()) return 100;
        if (m.is_gsim()) return 50;
        return 0;
      case BackendPolicy::GSIM_ONLY:
        if (m.is_gsim()) return 100;
        if (m.is_verilator()) return 50;
        return 0;
      case BackendPolicy::MIXED:
      default:
        break;
    }

    const bool prefer_gsim = (m.type == ModuleType::COMB);
    if (prefer_gsim) {
      if (m.is_gsim()) return 100;
      if (m.is_verilator()) return 50;
    } else {
      if (m.is_verilator()) return 100;
      if (m.is_gsim()) return 50;
    }
    return 0;
  };

  modules_.clear();
  std::vector<ModuleInfo> modules_list;
  for (auto& kv : candidates) {
    const std::string& mod_name = kv.first;
    std::vector<ModuleInfo>& vec = kv.second;
    if (vec.empty()) continue;

    // Pick best-scoring candidate.
    size_t best_i = 0;
    int best_s = score_candidate(vec[0]);
    for (size_t i = 1; i < vec.size(); ++i) {
      int s = score_candidate(vec[i]);
      if (s > best_s) {
        best_s = s;
        best_i = i;
      }
    }

    if (vec.size() > 1) {
      std::cout << "  [select] module=" << mod_name << " candidates=" << vec.size()
                << " chosen=" << vec[best_i].simulator_name
                << " type=" << vec[best_i].get_type_str() << "\n";
    }

    modules_[mod_name] = vec[best_i];
    modules_list.push_back(vec[best_i]);
  }

  // Sort for consistent ordering (helps deterministic output).
  std::sort(modules_list.begin(), modules_list.end(),
            [](const ModuleInfo& a, const ModuleInfo& b) { return a.module_name < b.module_name; });

  // Build connections
  std::cout << "\n=== Building Connections ===" << std::endl;
  conn_builder_ = new ConnectionBuilder();
  auto connections = conn_builder_->build(modules_list);
  total_connections_ = connections.size();

  std::cout << "  Total connections: " << total_connections_ << std::endl;

  return true;
}

bool CodeGenerator::generate_single_module_diff(const std::string& module_name,
                                                const std::string& output_file_base,
                                                int iters,
                                                unsigned long long seed,
                                                int max_width_bits) {
  std::cout << "\n=== Generating Single-Module Diff Runner ===" << std::endl;

  // Discover all modules from all simulators under modules_dir_.
  ModuleDiscoveryManager manager;
  auto discovery_results = manager.discover_all_modules(modules_dir_);
  manager.print_discovery_statistics();

  // Pick the target module from both Verilator and GSIM.
  bool found_v = false;
  bool found_g = false;
  ModuleInfo vinfo;
  ModuleInfo ginfo;

  for (const auto& r : discovery_results) {
    if (r.module_name != module_name) continue;
    std::unique_ptr<ModuleParser> parser = ModuleParserFactory::create(r.simulator_name);
    if (!parser) continue;
    ModuleInfo info = parser->parse(r.header_path);
    if (r.simulator_name == "Verilator" && !found_v) {
      vinfo = info;
      found_v = true;
    } else if (r.simulator_name == "GSIM" && !found_g) {
      ginfo = info;
      found_g = true;
    }
  }

  if (!found_v) {
    std::cerr << "Error: module '" << module_name << "' not found in Verilator outputs under " << modules_dir_ << "\n";
    return false;
  }
  if (!found_g) {
    std::cerr << "Error: module '" << module_name << "' not found in GSIM outputs under " << modules_dir_ << "\n";
    return false;
  }

  // Determine include directories (module folders).
  std::string verilator_dir = path_dirname(vinfo.header_path);
  std::string gsim_dir = path_dirname(ginfo.header_path);

  // Decide Verilator library path. Prefer V<module>__ALL.a if present.
  std::string v_all_a = verilator_dir + "/V" + module_name + "__ALL.a";
  std::string v_lib_a = verilator_dir + "/libV" + module_name + ".a";
  std::string verilator_lib = file_exists(v_all_a) ? v_all_a : v_lib_a;

  // For GSIM, we don't require a static library; we'll compile *.cpp in the module dir.
  std::string gsim_cpp_glob = gsim_dir + "/*.cpp";

  const std::string out_cpp = output_file_base + ".cpp";
  const std::string out_mk = output_file_base + ".mk";

  std::ofstream ocpp(out_cpp);
  if (!ocpp.is_open()) {
    std::cerr << "Failed to open output CPP file: " << out_cpp << "\n";
    return false;
  }

  // Generate diff runner C++ (supports both scalar <=64 and wide ports via 32-bit words).
  ocpp << "// ============================================================================\n";
  ocpp << "// Auto-generated by Corvusitor (diff-single)\n";
  ocpp << "// Module: " << module_name << "\n";
  ocpp << "// Iterations: " << iters << " Seed: " << seed << "\n";
  ocpp << "// NOTE: This runner supports scalar ports (<=64) and wide ports via 32-bit words.\n";
  ocpp << "// Diff max width: " << max_width_bits << " bits (wider ports are ignored)\n";
  ocpp << "// ============================================================================\n\n";
  ocpp << "#include \"verilated.h\"\n";
  ocpp << "#include \"" << path_basename(vinfo.header_path) << "\"\n";
  ocpp << "#include \"" << path_basename(ginfo.header_path) << "\"\n\n";
  ocpp << "#include <cstdint>\n#include <iostream>\n#include <random>\n\n";
  ocpp << "static inline uint64_t mask_u64(int w) {\n";
  ocpp << "  if (w <= 0) return 0;\n";
  ocpp << "  if (w >= 64) return ~0ull;\n";
  ocpp << "  return (1ull << w) - 1ull;\n";
  ocpp << "}\n\n";
  ocpp << "static inline uint32_t mask_u32(int w) {\n";
  ocpp << "  if (w <= 0) return 0;\n";
  ocpp << "  if (w >= 32) return 0xffffffffu;\n";
  ocpp << "  return (1u << w) - 1u;\n";
  ocpp << "}\n\n";
  ocpp << "int main(int /*argc*/, char** /*argv*/) {\n";
  ocpp << "  Verilated::randReset(0);\n";
  ocpp << "  " << vinfo.class_name << " v;\n";
  ocpp << "  " << ginfo.class_name << " g;\n";
  ocpp << "  std::mt19937_64 rng(" << seed << "ull);\n";
  ocpp << "  const int kIters = " << iters << ";\n\n";

  auto canon = [](std::string n) -> std::string {
    // Match Verilator/CIRCT encoding: "2F" -> "_" (used in extp_* ports).
    for (size_t pos = 0; (pos = n.find("2F", pos)) != std::string::npos;) {
      n.replace(pos, 2, "_");
      pos += 1;
    }
    return n;
  };

  struct VPortMeta {
    std::string field_name;  // actual C++ member name in V<module>
    int width;
  };

  // Build lookup by both raw name and canonicalized name, so we can match GSIM ports
  // (underscore form) to Verilator C++ members (2F-encoded form).
  std::map<std::string, VPortMeta> vports;
  for (const auto& p : vinfo.ports) {
    const int w = p.get_width();
    const std::string field = p.get_cpp_name();
    vports[p.name] = {field, w};
    const std::string c = canon(field);
    if (vports.find(c) == vports.end()) {
      vports[c] = {field, w};
    }
  }

  // Emit randomized loop; drive all matched inputs (including wide),
  // and compare matched outputs up to max_width_bits (0 => no width filter).
  ocpp << "  for (int i = 0; i < kIters; ++i) {\n";

  // Inputs
  for (const auto& p : ginfo.ports) {
    if (p.direction != PortDirection::INPUT) continue;
    int w = p.get_width();
    const std::string key = p.name;
    auto vit = vports.find(key);
    if (vit == vports.end()) continue;
    const std::string vname = vit->second.field_name;
    const std::string gname = p.get_cpp_name();

    // Keep clock/reset deterministic.
    if (p.name == "clock" || p.name == "clk" || p.name == "reset" || p.name == "rst") {
      ocpp << "    v." << vname << " = 0;\n";
      ocpp << "    g.set_" << gname << "(0);\n";
      continue;
    }

    // GSIM may emit width=0 for some signals; skip these to avoid undefined masks/shifts.
    if (w <= 0) {
      continue;
    }

    if (w <= 64) {
      ocpp << "    {\n";
      ocpp << "      const uint64_t m = mask_u64(" << w << ");\n";
      ocpp << "      const uint64_t raw = rng() & m;\n";
      ocpp << "      v." << vname << " = raw;\n";
      ocpp << "      g.set_" << gname << "(raw);\n";
      ocpp << "    }\n";
      continue;
    }

    // Wide: drive as 32-bit words and build a GSIM value via decltype(g.<port>).
    //
    // IMPORTANT: GSIM may report a *semantic* bitwidth that is smaller than the Verilator port bitwidth
    // (e.g. due to padding/alignment). In that case we must still fully initialize the Verilator
    // VlWide array to avoid leaving upper words uninitialized, which can cause immediate mismatches.
    const int vw_bits = vit->second.width;
    const int vwords = (vw_bits + 31) / 32;
    const int words = (w + 31) / 32;
    const int rem = w % 32;
    const int drive_words = (words < vwords) ? words : vwords;
    ocpp << "    {\n";
    if (vw_bits > 64) {
      ocpp << "      for (int wi = 0; wi < " << vwords << "; ++wi) v." << vname << "[wi] = 0;\n";
    } else {
      // Should be rare/unexpected, but keep generation robust if metadata is inconsistent.
      ocpp << "      v." << vname << " = 0;\n";
    }
    ocpp << "      using T = unsigned _BitInt(" << w << ");\n";
    ocpp << "      T gv = 0;\n";
    ocpp << "      for (int wi = 0; wi < " << drive_words << "; ++wi) {\n";
    ocpp << "        uint32_t w32 = static_cast<uint32_t>(rng());\n";
    if (rem != 0) {
      ocpp << "        if (wi == " << (words - 1) << ") w32 &= mask_u32(" << rem << ");\n";
    }
    ocpp << "        v." << vname << "[wi] = w32;\n";
    ocpp << "        gv |= (T(w32) << (wi * 32));\n";
    ocpp << "      }\n";
    ocpp << "      g.set_" << gname << "(gv);\n";
    ocpp << "    }\n";
    continue;
  }

  ocpp << "\n    v.eval();\n    g.step();\n\n";

  // Outputs compare
  ocpp << "    bool ok = true;\n";
  for (const auto& p : ginfo.ports) {
    if (p.direction != PortDirection::OUTPUT) continue;
    int w = p.get_width();
    if (w <= 0) continue;
    if (max_width_bits > 0 && w > max_width_bits) continue;
    const std::string key = p.name;
    auto vit = vports.find(key);
    if (vit == vports.end()) continue;
    const std::string vname = vit->second.field_name;
    const std::string gname = p.get_cpp_name();

    if (w <= 64) {
      ocpp << "    {\n";
      ocpp << "      const uint64_t m = mask_u64(" << w << ");\n";
      ocpp << "      const uint64_t vo = (uint64_t)(v." << vname << ") & m;\n";
      ocpp << "      const uint64_t go = (uint64_t)(g.get_" << gname << "()) & m;\n";
      ocpp << "      if (vo != go) {\n";
      ocpp << "        std::cerr << \"Mismatch at iter=\" << i << \" port=" << p.name << "\\n\";\n";
      ocpp << "        std::cerr << \"  verilator=\" << vo << \" gsim=\" << go << \"\\n\";\n";
      ocpp << "        ok = false;\n";
      ocpp << "      }\n";
      ocpp << "    }\n";
      continue;
    }

    const int vw_bits = vit->second.width;
    const int vwords = (vw_bits + 31) / 32;
    const int words = (w + 31) / 32;
    const int rem = w % 32;
    const int compare_words = (words < vwords) ? words : vwords;
    ocpp << "    {\n";
    ocpp << "      using T = unsigned _BitInt(" << w << ");\n";
    ocpp << "      const T gv = (T)g.get_" << gname << "();\n";
    // Be robust to width mismatches: never index past Verilator's VlWide length.
    // If widths disagree, report it and mark mismatch (but still compare the common prefix words).
    ocpp << "      if (" << vwords << " < " << words << ") {\n";
    ocpp << "        std::cerr << \"Width mismatch at iter=\" << i << \" port=" << p.name
         << " gsim_bits=" << w << " verilator_bits=" << vw_bits << "\\n\";\n";
    ocpp << "        ok = false;\n";
    ocpp << "      }\n";
    ocpp << "      for (int wi = 0; wi < " << compare_words << "; ++wi) {\n";
    ocpp << "        uint32_t vw = v." << vname << "[wi];\n";
    if (rem != 0) {
      ocpp << "        if (wi == " << (words - 1) << ") vw &= mask_u32(" << rem << ");\n";
    }
    ocpp << "        uint32_t gw = (uint32_t)((gv >> (wi * 32)) & 0xffffffffu);\n";
    if (rem != 0) {
      ocpp << "        if (wi == " << (words - 1) << ") gw &= mask_u32(" << rem << ");\n";
    }
    ocpp << "        if (vw != gw) {\n";
    ocpp << "          std::cerr << \"Mismatch at iter=\" << i << \" port=" << p.name << " word=\" << wi << \"\\n\";\n";
    ocpp << "          std::cerr << \"  verilator=\" << vw << \" gsim=\" << gw << \"\\n\";\n";
    ocpp << "          ok = false;\n";
    ocpp << "          break;\n";
    ocpp << "        }\n";
    ocpp << "      }\n";
    ocpp << "    }\n";
  }
  ocpp << "    if (!ok) return 1;\n";
  ocpp << "  }\n";
  ocpp << "  std::cout << \"PASS: " << module_name << " GSIM vs Verilator matched for \" << kIters << \" random vectors.\\n\";\n";
  ocpp << "  return 0;\n";
  ocpp << "}\n";
  ocpp.close();

  // Generate Makefile for building and running the diff runner.
  std::ofstream omk(out_mk);
  if (!omk.is_open()) {
    std::cerr << "Failed to open output Makefile: " << out_mk << "\n";
    return false;
  }

  omk << "# Auto-generated Makefile for Corvusitor diff-single\n";
  omk << "# Module: " << module_name << "\n\n";
  omk << "VERILATOR_ROOT ?= $(shell verilator --getenv VERILATOR_ROOT)\n";
  omk << "VERILATOR_INCLUDE = $(VERILATOR_ROOT)/include\n\n";
  omk << "MODULE_PATH = " << modules_dir_ << "\n";
  omk << "VERILATOR_DIR = " << verilator_dir << "\n";
  omk << "GSIM_DIR = " << gsim_dir << "\n\n";
  // NOTE: make defines a default CXX=g++, so we must override (not ?=) for GSIM BITINT support.
  omk << "CXX = clang++-19\n";
  omk << "CXXFLAGS = -std=c++20 -O2 -Wall -Wextra -g \\\n";
  omk << "           -I. -I$(VERILATOR_INCLUDE) -I$(VERILATOR_INCLUDE)/vltstd \\\n";
  omk << "           -I$(VERILATOR_DIR) -I$(GSIM_DIR)\n";
  omk << "LDLIBS = -lpthread\n\n";
  omk << "VERILATOR_RUNTIME = $(VERILATOR_INCLUDE)/verilated.cpp $(VERILATOR_INCLUDE)/verilated_threads.cpp\n";
  omk << "VERILATOR_LIB = " << verilator_lib << "\n";
  omk << "GSIM_SRCS = $(wildcard $(GSIM_DIR)/*.cpp)\n\n";
  omk << "TARGET ?= diff_runner\n";
  omk << "SOURCES = " << path_basename(out_cpp) << " $(GSIM_SRCS) $(VERILATOR_RUNTIME)\n\n";
  omk << ".PHONY: all test clean\n\n";
  omk << "all: $(TARGET)\n\n";
  omk << "$(TARGET): $(SOURCES)\n";
  omk << "\t$(CXX) $(CXXFLAGS) -o $@ $(SOURCES) $(VERILATOR_LIB) $(LDLIBS)\n\n";
  omk << "test: $(TARGET)\n";
  omk << "\t./$(TARGET)\n\n";
  omk << "clean:\n";
  omk << "\trm -f $(TARGET) *.o\n";
  omk.close();

  std::cout << "Generated diff runner:\n";
  std::cout << "  - " << out_cpp << "\n";
  std::cout << "  - " << out_mk << "\n";
  return true;
}

std::string CodeGenerator::generate_assignment(const PortConnection& conn) {
  std::ostringstream oss;

  const int w = conn.width;
  const bool is_wide = (conn.width_type == PortWidthType::VL_W);
  const int words = words_for_width(w);
  const int rem = w % 32;

  // TOP -> module inputs
  if (conn.is_top_level_input) {
    for (const auto* receiver : conn.receiver_modules) {
      const PortInfo* rp = find_port_by_logical_name(receiver, conn.port_name);
      const std::string recv_ptr = "m_" + receiver->instance_name;
      const std::string rcpp = rp ? rp->get_cpp_name() : conn.port_name;

      if (!is_wide) {
        if (receiver->is_gsim()) {
          oss << "    " << recv_ptr << "->set_" << rcpp
              << "(((uint64_t)" << conn.port_name << ") & _corvus_mask_u64(" << w << "));\n";
        } else {
          oss << "    " << recv_ptr << "->" << rcpp << " = " << conn.port_name << ";\n";
        }
      } else {
        if (!receiver->is_gsim()) {
          const int rwords = rp ? rp->array_size : words;
          oss << "    for (int wi = 0; wi < " << rwords << "; ++wi) " << recv_ptr << "->" << rcpp
              << "[wi] = " << conn.port_name << "[wi];\n";
        } else {
          oss << "    {\n";
          oss << "      using T = unsigned _BitInt(" << w << ");\n";
          oss << "      " << recv_ptr << "->set_" << rcpp << "(";
          for (int wi = 0; wi < words; ++wi) {
            if (wi != 0) oss << " | ";
            oss << "(T(";
            if (rem != 0 && wi == (words - 1)) {
              oss << "((uint32_t)" << conn.port_name << "[" << wi << "] & _corvus_mask_u32(" << rem << "))";
            } else {
              oss << "(uint32_t)" << conn.port_name << "[" << wi << "]";
            }
            oss << ") << " << (wi * 32) << ")";
          }
          oss << ");\n";
          oss << "    }\n";
        }
      }
    }
    return oss.str();
  }

  // module outputs -> TOP
  if (conn.is_top_level_output) {
    if (!conn.driver_module) return oss.str();
    const ModuleInfo* driver = conn.driver_module;
    const PortInfo* dp = find_port_by_logical_name(driver, conn.port_name);
    const std::string driver_ptr = "m_" + driver->instance_name;
    const std::string dcpp = dp ? dp->get_cpp_name() : conn.port_name;

    if (!is_wide) {
      if (driver->is_gsim()) {
        oss << "    " << conn.port_name << " = (uint64_t)(" << driver_ptr << "->get_" << dcpp
            << "()) & _corvus_mask_u64(" << w << ");\n";
      } else {
        oss << "    " << conn.port_name << " = " << driver_ptr << "->" << dcpp << ";\n";
      }
    } else {
      if (!driver->is_gsim()) {
        const int dwords = dp ? dp->array_size : words;
        oss << "    for (int wi = 0; wi < " << dwords << "; ++wi) " << conn.port_name
            << "[wi] = " << driver_ptr << "->" << dcpp << "[wi];\n";
      } else {
        oss << "    {\n";
        oss << "      using T = unsigned _BitInt(" << w << ");\n";
        oss << "      for (int wi = 0; wi < " << words << "; ++wi) {\n";
        oss << "        uint32_t w32 = (uint32_t)(((T)" << driver_ptr << "->get_" << dcpp << "() >> (wi * 32)) & 0xffffffffu);\n";
        if (rem != 0) {
          oss << "        if (wi == " << (words - 1) << ") w32 &= _corvus_mask_u32(" << rem << ");\n";
        }
        oss << "        " << conn.port_name << "[wi] = w32;\n";
        oss << "      }\n";
        oss << "    }\n";
      }
    }
    return oss.str();
  }

  // Normal connection: driver -> receivers
  if (!conn.driver_module || conn.receiver_modules.empty()) {
    return "    // ERROR: Invalid connection\n";
  }

  const ModuleInfo* driver = conn.driver_module;
  const PortInfo* dp = find_port_by_logical_name(driver, conn.port_name);
  const std::string driver_ptr = "m_" + driver->instance_name;
  const std::string dcpp = dp ? dp->get_cpp_name() : conn.port_name;

  for (const auto* receiver : conn.receiver_modules) {
    const PortInfo* rp = find_port_by_logical_name(receiver, conn.port_name);
    const std::string recv_ptr = "m_" + receiver->instance_name;
    const std::string rcpp = rp ? rp->get_cpp_name() : conn.port_name;

    if (!is_wide) {
      const std::string read_expr =
        driver->is_gsim()
          ? ("(((uint64_t)" + driver_ptr + "->get_" + dcpp + "()) & _corvus_mask_u64(" + std::to_string(w) + "))")
          : ("(((uint64_t)" + driver_ptr + "->" + dcpp + ") & _corvus_mask_u64(" + std::to_string(w) + "))");

      if (receiver->is_gsim()) {
        oss << "    " << recv_ptr << "->set_" << rcpp << "(" << read_expr << ");\n";
      } else {
        oss << "    " << recv_ptr << "->" << rcpp << " = " << read_expr << ";\n";
    }
      continue;
    }

    // Wide (>= 65b): connect via 32-bit words / _BitInt.
    if (!driver->is_gsim() && !receiver->is_gsim()) {
      const int dwords = dp ? dp->array_size : words;
      const int rwords = rp ? rp->array_size : words;
      const int n = (dwords < rwords) ? dwords : rwords;
      oss << "    for (int wi = 0; wi < " << n << "; ++wi) " << recv_ptr << "->" << rcpp
          << "[wi] = " << driver_ptr << "->" << dcpp << "[wi];\n";
      continue;
    }

    if (!driver->is_gsim() && receiver->is_gsim()) {
      const int dwords = dp ? dp->array_size : words;
      const int drive_words = (dwords < words) ? dwords : words;
      oss << "    {\n";
      oss << "      using T = unsigned _BitInt(" << w << ");\n";
      oss << "      " << recv_ptr << "->set_" << rcpp << "(";
      for (int wi = 0; wi < drive_words; ++wi) {
        if (wi != 0) oss << " | ";
        oss << "(T(";
        if (rem != 0 && wi == (words - 1)) {
          oss << "((uint32_t)" << driver_ptr << "->" << dcpp << "[" << wi << "] & _corvus_mask_u32(" << rem << "))";
        } else {
          oss << "(uint32_t)" << driver_ptr << "->" << dcpp << "[" << wi << "]";
        }
        oss << ") << " << (wi * 32) << ")";
      }
      oss << ");\n";
      oss << "    }\n";
      continue;
    }

    if (driver->is_gsim() && !receiver->is_gsim()) {
      const int rwords = rp ? rp->array_size : words;
      oss << "    {\n";
      oss << "      using T = unsigned _BitInt(" << w << ");\n";
      oss << "      for (int wi = 0; wi < " << rwords << "; ++wi) " << recv_ptr << "->" << rcpp << "[wi] = 0;\n";
      oss << "      for (int wi = 0; wi < " << words << " && wi < " << rwords << "; ++wi) {\n";
      oss << "        uint32_t w32 = (uint32_t)(((T)" << driver_ptr << "->get_" << dcpp << "() >> (wi * 32)) & 0xffffffffu);\n";
      if (rem != 0) {
        oss << "        if (wi == " << (words - 1) << ") w32 &= _corvus_mask_u32(" << rem << ");\n";
      }
      oss << "        " << recv_ptr << "->" << rcpp << "[wi] = w32;\n";
      oss << "      }\n";
      oss << "    }\n";
      continue;
    }

    // driver GSIM -> receiver GSIM
    oss << "    " << recv_ptr << "->set_" << rcpp << "(" << driver_ptr << "->get_" << dcpp << "());\n";
  }

  return oss.str();
}

std::string CodeGenerator::generate_propagate_function(
  const std::vector<PortConnection>& connections,
  const std::string& function_name,
  const std::string& comment) {

  std::ostringstream oss;

  // Function header
  oss << "void VCorvusTopWrapper::" << function_name << "() {\n";
  oss << "    // " << comment << "\n";
  oss << "    // Auto-generated: " << connections.size() << " connections\n";
  oss << "\n";

  if (connections.empty()) {
    oss << "    // No connections\n";
  } else {
    // Group by module type for better organization
    std::vector<const PortConnection*> comb_to_seq;
    std::vector<const PortConnection*> seq_to_comb;
    std::vector<const PortConnection*> comb_to_ext;
    std::vector<const PortConnection*> ext_to_comb;
    std::vector<const PortConnection*> top_inputs;
    std::vector<const PortConnection*> top_outputs;

    for (const auto& conn : connections) {
      if (conn.is_top_level_input) {
        top_inputs.push_back(&conn);
      } else if (conn.is_top_level_output) {
        top_outputs.push_back(&conn);
      } else if (conn.driver_module && !conn.receiver_modules.empty()) {
        auto driver_type = conn.driver_module->type;
        auto receiver_type = conn.receiver_modules[0]->type;

        if (driver_type == ModuleType::COMB && receiver_type == ModuleType::SEQ) {
          comb_to_seq.push_back(&conn);
        } else if (driver_type == ModuleType::SEQ && receiver_type == ModuleType::COMB) {
          seq_to_comb.push_back(&conn);
        } else if (driver_type == ModuleType::COMB && receiver_type == ModuleType::EXTERNAL) {
          comb_to_ext.push_back(&conn);
        } else if (driver_type == ModuleType::EXTERNAL && receiver_type == ModuleType::COMB) {
          ext_to_comb.push_back(&conn);
        }
      }
    }

    // Generate organized code
    auto write_group = [&](const std::vector<const PortConnection*>& group, const std::string& title) {
      if (!group.empty()) {
        oss << "    // " << title << " (" << group.size() << " connections)\n";
        for (const auto* conn : group) {
          oss << generate_assignment(*conn);
        }
        oss << "\n";
      }
    };

    write_group(top_inputs, "Top-level inputs");
    write_group(comb_to_seq, "COMB -> SEQ");
    write_group(seq_to_comb, "SEQ -> COMB");
    write_group(comb_to_ext, "COMB -> EXTERNAL");
    write_group(ext_to_comb, "EXTERNAL -> COMB");
    write_group(top_outputs, "Top-level outputs");
  }

  oss << "}\n";

  return oss.str();
}

std::string CodeGenerator::generate_port_declarations(
  const std::vector<PortConnection>& top_inputs,
  const std::vector<PortConnection>& top_outputs) {

  std::ostringstream oss;

  oss << "    // ========================================================================\n";
  oss << "    // Top-level Input/Output Ports (Auto-generated)\n";
  oss << "    // Similar to Verilator-generated interface\n";
  oss << "    // ========================================================================\n";
  oss << "\n";

  // Generate input declarations
  if (!top_inputs.empty()) {
    oss << "    // Inputs (" << top_inputs.size() << " ports)\n";
    std::set<std::string> generated_inputs;

    for (const auto& conn : top_inputs) {
      if (!conn.is_top_level_input) continue;
      if (generated_inputs.find(conn.port_name) != generated_inputs.end()) continue;
      generated_inputs.insert(conn.port_name);

      // Find port type from first receiver
      if (conn.receiver_modules.empty()) continue;
      const PortInfo* port_info = nullptr;
      for (const auto& port : conn.receiver_modules[0]->ports) {
        if (port.name == conn.port_name) {
          port_info = &port;
          break;
        }
      }
      if (!port_info) continue;

      std::string cpp_type = port_info->get_cpp_type();

      // Handle VlWide types
      if (port_info->width_type == PortWidthType::VL_W) {
        oss << "    VlWide<" << port_info->array_size << "> " << conn.port_name << ";\n";
      } else {
        oss << "    " << cpp_type << " " << conn.port_name << ";\n";
      }
    }
    oss << "\n";
  }

  // Generate output declarations
  if (!top_outputs.empty()) {
    oss << "    // Outputs (" << top_outputs.size() << " ports)\n";
    std::set<std::string> generated_outputs;

    for (const auto& conn : top_outputs) {
      if (!conn.driver_module || !conn.is_top_level_output) continue;
      if (generated_outputs.find(conn.port_name) != generated_outputs.end()) continue;
      generated_outputs.insert(conn.port_name);

      // Find port type from driver module
      const PortInfo* port_info = nullptr;
      for (const auto& port : conn.driver_module->ports) {
        if (port.name == conn.port_name) {
          port_info = &port;
          break;
        }
      }
      if (!port_info) continue;

      std::string cpp_type = port_info->get_cpp_type();

      // Handle VlWide types
      if (port_info->width_type == PortWidthType::VL_W) {
        oss << "    VlWide<" << port_info->array_size << "> " << conn.port_name << ";\n";
      } else {
        oss << "    " << cpp_type << " " << conn.port_name << ";\n";
      }
    }
  }

  return oss.str();
}

// 用于顶层端口数字后缀排序
struct TopPortName {
  std::string base_name;
  size_t index;
};

TopPortName parseTopPortName(const std::string &s) {
  TopPortName result;
  int i = s.length() - 1;
  while (i >= 0 && std::isdigit(s[i])) i--;
  result.base_name = s.substr(0, i + 1);
  result.index = (i + 1 < s.length()) ? std::stoi(s.substr(i + 1)) : 0;
  return result;
}

bool compareTopPortNames(const PortConnection a, const PortConnection b) {
  TopPortName ta = parseTopPortName(a.port_name);
  TopPortName tb = parseTopPortName(b.port_name);
  if (ta.base_name != tb.base_name) return ta.base_name < tb.base_name;
  return ta.index < tb.index;
}

bool CodeGenerator::generate_all(const std::string& output_file_base) {
  std::cout << "\n=== Generating Connection Code ===" << std::endl;

  if (!conn_builder_) {
    std::cerr << "Error: Connection builder not initialized. Call load_data() first." << std::endl;
    return false;
  }

  const std::string output_cpp_file = output_file_base + ".cpp";
  const std::string output_h_file = output_file_base + ".h";

  // Open output files
  std::ofstream out_cpp(output_cpp_file);
  if (!out_cpp.is_open()) {
    std::cerr << "Failed to open output CPP file: " << output_cpp_file << std::endl;
    return false;
  }

  std::ofstream out_h(output_h_file);
  if (!out_h.is_open()) {
    std::cerr << "Failed to open output H file: " << output_h_file << std::endl;
    return false;
  }

  // Get header guard name from filename
  std::string header_guard = "VCORVUS_TOP_WRAPPER_GENERATED_H";

  // H file: Generate complete standalone header
  out_h << "// ============================================================================\n";
  out_h << "// Auto-generated by CodeGenerator\n";
  out_h << "// Multi-Module Wrapper - Standalone Header\n";
  out_h << "// Generated: " << __DATE__ << " " << __TIME__ << "\n";
  out_h << "// ============================================================================\n";
  out_h << "\n";
  out_h << "#ifndef " << header_guard << "\n";
  out_h << "#define " << header_guard << "\n";
  out_h << "\n";
  out_h << "// Common includes\n";
  out_h << "#include \"verilated.h\"\n";
  out_h << "#include <cstdint>\n";
  out_h << "\n";
  out_h << "// Auto-generated: Include module headers from selected backends (Verilator/GSIM)\n";

  for (const auto& pair : modules_) {
    const ModuleInfo& m = pair.second;
    out_h << "#include \"" << path_basename(m.header_path) << "\"\n";
  }
  out_h << "\n";
  out_h << "// ============================================================================\n";
  out_h << "// VCorvusTopWrapper - Main wrapper class for multi-module design\n";
  out_h << "// ============================================================================\n";
  out_h << "class VCorvusTopWrapper {\n";
  out_h << "public:\n";
  out_h << "    // Constructor & Destructor\n";
  out_h << "    VCorvusTopWrapper();\n";
  out_h << "    ~VCorvusTopWrapper();\n";
  out_h << "\n";
  out_h << "    // Simulation control\n";
  out_h << "    void eval();  // Evaluate one cycle\n";
  out_h << "\n";

  // CPP file header
  out_cpp << "// ============================================================================\n";
  out_cpp << "// Auto-generated by CodeGenerator\n";
  out_cpp << "// Multi-Module Wrapper Connection Code (Implementation)\n";
  out_cpp << "// Generated: " << __DATE__ << " " << __TIME__ << "\n";
  out_cpp << "// ============================================================================\n";
  out_cpp << "\n";
  out_cpp << "#include \"" << output_h_file << "\"\n";
  out_cpp << "#include <cstdint>\n";
  out_cpp << "\n";
  out_cpp << "#ifndef CORVUS_GSIM_COMB_STEPS\n";
  out_cpp << "#define CORVUS_GSIM_COMB_STEPS 1\n";
  out_cpp << "#endif\n";
  out_cpp << "\n";
  // (debug instrumentation removed)\n";
  out_cpp << "static inline uint64_t _corvus_mask_u64(int w) {\n";
  out_cpp << "  if (w <= 0) return 0;\n";
  out_cpp << "  if (w >= 64) return ~0ull;\n";
  out_cpp << "  return (1ull << w) - 1ull;\n";
  out_cpp << "}\n";
  out_cpp << "static inline uint32_t _corvus_mask_u32(int w) {\n";
  out_cpp << "  if (w <= 0) return 0;\n";
  out_cpp << "  if (w >= 32) return 0xffffffffu;\n";
  out_cpp << "  return (1u << w) - 1u;\n";
  out_cpp << "}\n";
  out_cpp << "\n";
  out_cpp << "// ============================================================================\n";
  out_cpp << "// Constructor & Destructor\n";
  out_cpp << "// ============================================================================\n";
  out_cpp << "VCorvusTopWrapper::VCorvusTopWrapper() {\n";

  // Generate constructor initialization from modules_
  for (const auto& pair : modules_) {
    const ModuleInfo& m = pair.second;
    out_cpp << "    m_" << m.instance_name << " = new " << m.class_name << "();\n";
  }

  out_cpp << "}\n";
  out_cpp << "\n";
  out_cpp << "VCorvusTopWrapper::~VCorvusTopWrapper() {\n";

  // Generate destructor cleanup from modules_
  for (const auto& pair : modules_) {
    const std::string& instance_name = pair.second.instance_name;
    out_cpp << "    delete m_" << instance_name << ";\n";
  }

  out_cpp << "}\n";
  out_cpp << "\n";
  out_cpp << "// ============================================================================\n";
  out_cpp << "// Main Evaluation Function\n";
  out_cpp << "// ============================================================================\n";

  // Collect modules by type
  std::vector<const ModuleInfo*> comb_modules;
  std::vector<const ModuleInfo*> seq_modules;
  std::vector<const ModuleInfo*> external_modules;

  for (const auto& pair : modules_) {
    const ModuleInfo& module = pair.second;
    switch (module.type) {
      case ModuleType::COMB:
        comb_modules.push_back(&module);
        break;
      case ModuleType::SEQ:
        seq_modules.push_back(&module);
        break;
      case ModuleType::EXTERNAL:
        external_modules.push_back(&module);
        break;
    }
  }

  out_cpp << "void VCorvusTopWrapper::eval() {\n";
  out_cpp << "    // Mixed-backend scheduling note:\n";
  out_cpp << "    // We do a two-phase settle so that after SEQ updates (posedge),\n";
  out_cpp << "    // COMB is re-evaluated and top-level outputs reflect the updated state.\n";
  out_cpp << "    //\n";
  out_cpp << "    // Phase 0: drive COMB inputs from TOP/SEQ/EXTERNAL\n";
  out_cpp << "    propagate_inputs_to_comb();\n";
  out_cpp << "    propagate_seq_to_comb();\n";
  out_cpp << "    propagate_external_to_comb();\n";
  out_cpp << "\n";

  // Evaluate COMB modules
  if (!comb_modules.empty()) {
    out_cpp << "    // Phase 1: evaluate COMB modules (" << comb_modules.size() << " modules)\n";
    for (const auto* module : comb_modules) {
      if (module->is_gsim()) {
        out_cpp << "    for (int __i = 0; __i < CORVUS_GSIM_COMB_STEPS; ++__i) m_" << module->instance_name << "->step();\n";
      } else {
      out_cpp << "    m_" << module->instance_name << "->eval();\n";
      }
    }
    out_cpp << "\n";
  }

  out_cpp << "    // Phase 1.5: snapshot top outputs from COMB (pre-SEQ)\n";
  out_cpp << "    propagate_comb_to_outputs();\n";
  out_cpp << "\n";
  out_cpp << "    // Phase 2: propagate COMB outputs into SEQ/EXTERNAL\n";
  out_cpp << "    propagate_comb_to_seq();\n";
  out_cpp << "    propagate_comb_to_external();\n";
  out_cpp << "\n";

  // Evaluate SEQ modules
  if (!seq_modules.empty()) {
    out_cpp << "    // Phase 3: evaluate SEQ modules (" << seq_modules.size() << " modules)\n";
    for (const auto* module : seq_modules) {
      out_cpp << "    m_" << module->instance_name << "->" << (module->is_gsim() ? "step" : "eval") << "();\n";
    }
    out_cpp << "\n";
  }

  // Evaluate EXTERNAL modules
  if (!external_modules.empty()) {
    out_cpp << "    // Phase 3: evaluate EXTERNAL modules (" << external_modules.size() << " modules)\n";
    for (const auto* module : external_modules) {
      out_cpp << "    m_" << module->instance_name << "->" << (module->is_gsim() ? "step" : "eval") << "();\n";
    }
    out_cpp << "\n";
  }

  out_cpp << "    // Phase 4: re-drive COMB from updated SEQ/EXTERNAL and re-evaluate COMB\n";
  out_cpp << "    propagate_seq_to_comb();\n";
  out_cpp << "    propagate_external_to_comb();\n";
  if (!comb_modules.empty()) {
    out_cpp << "    // Phase 4: evaluate COMB modules (post-SEQ)\n";
    for (const auto* module : comb_modules) {
      if (module->is_gsim()) {
        out_cpp << "    for (int __i = 0; __i < CORVUS_GSIM_COMB_STEPS; ++__i) m_" << module->instance_name << "->step();\n";
      } else {
      out_cpp << "    m_" << module->instance_name << "->eval();\n";
      }
    }
    out_cpp << "\n";
  }
  out_cpp << "    // Phase 5: final top-level outputs from COMB (post-SEQ)\n";
  out_cpp << "    propagate_comb_to_outputs();\n";

  out_cpp << "}\n";
  out_cpp << "\n";
  out_cpp << "// ============================================================================\n";
  out_cpp << "// Connection Propagation Functions\n";
  out_cpp << "// ============================================================================\n";
  out_cpp << "\n";

  // Get all connections
  std::vector<ModuleInfo> modules_list;
  for (const auto& pair : modules_) {
    modules_list.push_back(pair.second);
  }
  auto all_connections = conn_builder_->build(modules_list);

  // Separate connections by type
  std::vector<PortConnection> comb_to_seq, seq_to_comb, comb_to_ext, ext_to_comb;
  std::vector<PortConnection> top_inputs, top_outputs;

  for (const auto& conn : all_connections) {
    if (conn.is_top_level_input) {
      top_inputs.push_back(conn);
    } else if (conn.is_top_level_output) {
      top_outputs.push_back(conn);
    } else if (conn.driver_module && !conn.receiver_modules.empty()) {
      auto driver_type = conn.driver_module->type;
      auto receiver_type = conn.receiver_modules[0]->type;

      if (driver_type == ModuleType::COMB && receiver_type == ModuleType::SEQ) {
        comb_to_seq.push_back(conn);
      } else if (driver_type == ModuleType::SEQ && receiver_type == ModuleType::COMB) {
        seq_to_comb.push_back(conn);
      } else if (driver_type == ModuleType::COMB && receiver_type == ModuleType::EXTERNAL) {
        comb_to_ext.push_back(conn);
      } else if (driver_type == ModuleType::EXTERNAL && receiver_type == ModuleType::COMB) {
        ext_to_comb.push_back(conn);
      }
    }
  }

  std::sort(top_inputs.begin(), top_inputs.end(), compareTopPortNames);
  std::sort(top_outputs.begin(), top_outputs.end(), compareTopPortNames);

  // Generate port declarations for header file
  std::cout << "  Generating port declarations for header... (" << top_inputs.size()
            << " inputs, " << top_outputs.size() << " outputs)" << std::endl;
  out_h << "    // ========================================================================\n";
  out_h << "    // Top-level Input/Output Ports (Verilator-style public interface)\n";
  out_h << "    // ========================================================================\n";
  out_h << "\n";
  out_h << generate_port_declarations(top_inputs, top_outputs);
  out_h << "\n";
  out_h << "private:\n";
  out_h << "    // ========================================================================\n";
  out_h << "    // Internal Module Instances (Auto-generated)\n";
  out_h << "    // ========================================================================\n";

  // Generate member variable declarations from modules_
  for (const auto& pair : modules_) {
    const ModuleInfo& m = pair.second;
    out_h << "    " << m.class_name << "* m_" << m.instance_name << ";\n";
  }

  out_h << "\n";
  out_h << "    // ========================================================================\n";
  out_h << "    // Connection Propagation Functions\n";
  out_h << "    // ========================================================================\n";
  out_h << "    void propagate_inputs_to_comb();\n";
  out_h << "    void propagate_comb_to_seq();\n";
  out_h << "    void propagate_seq_to_comb();\n";
  out_h << "    void propagate_comb_to_external();\n";
  out_h << "    void propagate_external_to_comb();\n";
  out_h << "    void propagate_comb_to_outputs();\n";

  // Chunk helpers for large propagations (declared only when needed).
  constexpr size_t kChunkSize = 512;
  const size_t comb_to_seq_chunks = (comb_to_seq.size() + kChunkSize - 1) / kChunkSize;
  const size_t seq_to_comb_chunks = (seq_to_comb.size() + kChunkSize - 1) / kChunkSize;
  if (comb_to_seq.size() > kChunkSize) {
    for (size_t i = 0; i < comb_to_seq_chunks; ++i) {
      out_h << "    void propagate_comb_to_seq_chunk" << i << "();\n";
    }
  }
  if (seq_to_comb.size() > kChunkSize) {
    for (size_t i = 0; i < seq_to_comb_chunks; ++i) {
      out_h << "    void propagate_seq_to_comb_chunk" << i << "();\n";
    }
  }

  out_h << "};\n";
  out_h << "\n";
  out_h << "#endif // " << header_guard << "\n";

  // Generate each propagate function
  std::cout << "  Generating propagate_inputs_to_comb()... (" << top_inputs.size() << " inputs)" << std::endl;
  out_cpp << generate_propagate_function(top_inputs, "propagate_inputs_to_comb",
                     "TOP -> COMB: " + std::to_string(top_inputs.size()) + " inputs");
  out_cpp << "\n";

  std::cout << "  Generating propagate_comb_to_seq()... (" << comb_to_seq.size() << " connections)" << std::endl;
  out_cpp << generate_chunked_propagate(comb_to_seq, "propagate_comb_to_seq",
                     "COMB -> SEQ: " + std::to_string(comb_to_seq.size()) + " connections", kChunkSize);
  out_cpp << "\n";

  std::cout << "  Generating propagate_seq_to_comb()... (" << seq_to_comb.size() << " connections)" << std::endl;
  out_cpp << generate_chunked_propagate(seq_to_comb, "propagate_seq_to_comb",
                     "SEQ -> COMB: " + std::to_string(seq_to_comb.size()) + " connections", kChunkSize);
  out_cpp << "\n";

  std::cout << "  Generating propagate_comb_to_external()... (" << comb_to_ext.size() << " connections)" << std::endl;
  out_cpp << generate_propagate_function(comb_to_ext, "propagate_comb_to_external",
                     "COMB -> EXTERNAL: " + std::to_string(comb_to_ext.size()) + " connections");
  out_cpp << "\n";

  std::cout << "  Generating propagate_external_to_comb()... (" << ext_to_comb.size() << " connections)" << std::endl;
  out_cpp << generate_propagate_function(ext_to_comb, "propagate_external_to_comb",
                     "EXTERNAL -> COMB: " + std::to_string(ext_to_comb.size()) + " connections");
  out_cpp << "\n";

  std::cout << "  Generating propagate_comb_to_outputs()... (" << top_outputs.size() << " outputs)" << std::endl;
  out_cpp << generate_propagate_function(top_outputs, "propagate_comb_to_outputs",
                     "COMB -> TOP: " + std::to_string(top_outputs.size()) + " outputs");
  out_cpp << "\n";

  out_cpp.close();
  out_h.close();

  // Generate Makefile
  std::string output_dir;
  size_t last_slash = output_cpp_file.rfind('/');
  if (last_slash != std::string::npos) {
    output_dir = output_cpp_file.substr(0, last_slash + 1);
  } else {
    output_dir = "./";
  }

  // Extract just the filenames (without path)
  std::string cpp_filename = output_cpp_file.substr(last_slash + 1);
  std::string h_filename = output_h_file.substr(output_h_file.rfind('/') + 1);
  std::string makefile_path = output_dir + "Makefile.wrapper";

  if (!generate_makefile(output_file_base)) {
    std::cerr << "Warning: Failed to generate Makefile" << std::endl;
  }

  std::cout << "\n=== Generation Complete ===" << std::endl;
  std::cout << "  Output CPP file: " << output_cpp_file << std::endl;
  std::cout << "  Output H file: " << output_h_file << std::endl;
  std::cout << "  Output Makefile: " << makefile_path << std::endl;
  std::cout << "  Total connections: " << all_connections.size() << std::endl;
  std::cout << "    - TOP -> COMB: " << top_inputs.size() << std::endl;
  std::cout << "    - COMB -> SEQ: " << comb_to_seq.size() << std::endl;
  std::cout << "    - SEQ -> COMB: " << seq_to_comb.size() << std::endl;
  std::cout << "    - COMB -> EXTERNAL: " << comb_to_ext.size() << std::endl;
  std::cout << "    - EXTERNAL -> COMB: " << ext_to_comb.size() << std::endl;
  std::cout << "    - COMB -> TOP: " << top_outputs.size() << std::endl;
  std::cout << "  VlWide ports: " << vlwide_ports_ << std::endl;

  return true;
}

void CodeGenerator::print_statistics() const {
  std::cout << "\n=== Code Generation Statistics ===" << std::endl;
  std::cout << "  Total connections: " << total_connections_ << std::endl;
  std::cout << "  VlWide ports: " << vlwide_ports_ << std::endl;
  std::cout << "  Top-level outputs: " << top_outputs_ << std::endl;
}

bool CodeGenerator::is_vlwide_type(const std::string& type_str) const {
  return type_str.find("VlWide<") != std::string::npos ||
         type_str.find("VlWideArray<") != std::string::npos;
}

int CodeGenerator::extract_vlwide_width(const std::string& type_str) const {
  std::regex width_regex(R"(VlWide(?:Array)?<(\d+)>)");
  std::smatch match;
  if (std::regex_search(type_str, match, width_regex)) {
    return std::stoi(match[1].str());
  }
  return 0;
}

bool CodeGenerator::generate_makefile(const std::string& output_file_base) {
  std::cout << "\n=== Generating Makefile ===" << std::endl;

  const std::string output_makefile = output_file_base + ".mk";
  const std::string wrapper_cpp_file = output_file_base + ".cpp";
  const std::string wrapper_h_file = output_file_base + ".h";

  std::ofstream out(output_makefile);
  if (!out.is_open()) {
    std::cerr << "Failed to open output Makefile: " << output_makefile << std::endl;
    return false;
  }

  const std::string module_path = to_abs_path(modules_dir_);
  bool has_gsim = false;
  bool gsim_needs_src_any = false;
  for (const auto& kv : modules_) {
    if (kv.second.is_gsim()) {
      has_gsim = true;
      // If any GSIM module lacks a prebuilt lib, we will compile its generated sources.
      const ModuleInfo& m = kv.second;
      const std::string dir_abs = module_path + "/gsim-compile-" + m.module_name;
      const std::string gsim_lib_abs = dir_abs + "/lib" + m.class_name + ".a";
      if (!file_exists(gsim_lib_abs)) gsim_needs_src_any = true;
    }
  }

  out << "# Auto-generated Makefile for VCorvusTopWrapper\n";
  out << "# Generated: " << __DATE__ << " " << __TIME__ << "\n";
  out << "\n";
  out << "# Verilator configuration\n";
  out << "_CORVUS_VERILATOR_ROOT ?= $(shell verilator --getenv VERILATOR_ROOT)\n";
  out << "_CORVUS_VERILATOR_INCLUDE = $(_CORVUS_VERILATOR_ROOT)/include\n";
  out << "\n";
  out << "# Module directories\n";
  out << "_CORVUS_MODULE_PATH = " << module_path << "\n";

  // Generate module directory variables (mixed backends)
  for (const auto& pair : modules_) {
    const ModuleInfo& m = pair.second;
    std::string var_name = m.module_name;
    std::transform(var_name.begin(), var_name.end(), var_name.begin(), ::toupper);
    out << "_CORVUS_" << var_name << "_DIR = $(_CORVUS_MODULE_PATH)/"
        << (m.is_gsim() ? "gsim-compile-" : "verilator-compile-") << m.module_name << "\n";
  }

  out << "\n";
  out << "# Compiler settings\n";
  if (has_gsim) {
    // GSIM-generated headers may require clang with BITINT support.
    out << "_CORVUS_CXX ?= clang++-19\n";
    if (gsim_needs_src_any) {
      // Compiling GSIM-generated sources can be very heavy; prefer low optimization for reliability.
      out << "_CORVUS_CXXFLAGS = -std=c++20 -O0 -g0 -w \\\n";
    } else {
      out << "_CORVUS_CXXFLAGS = -std=c++20 -O2 -Wall -Wextra -g \\\n";
    }
  } else {
  out << "_CORVUS_CXX ?= g++\n";
    out << "_CORVUS_CXXFLAGS = -std=c++14 -O2 -Wall -Wextra -g \\\n";
  }
  out << "                   -I. \\\n";
  out << "                   -I$(_CORVUS_VERILATOR_INCLUDE) \\\n";
  out << "                   -I$(_CORVUS_VERILATOR_INCLUDE)/vltstd";

  // Add module include paths
  for (const auto& pair : modules_) {
    const std::string& module_name = pair.first;
    std::string var_name = module_name;
    std::transform(var_name.begin(), var_name.end(), var_name.begin(), ::toupper);
    out << " \\\n                   -I$(" << "_CORVUS_" << var_name << "_DIR)";
  }
  out << "\n";

  out << "_CORVUS_LDLIBS = -lpthread -lz\n";

  // Extra include flags provided by users
  out << "\n";
  out << "# User provided extra include flags\n";
  out << "_CORVUS_USER_INCLUDE_FLAGS ?=\n";
  out << "_CORVUS_CXXFLAGS += $(_CORVUS_USER_INCLUDE_FLAGS)\n";

  // Extra macro flags provided by users
  out << "\n";
  out << "# User provided extra macro flags\n";
  out << "_CORVUS_USER_MACRO_FLAGS ?=\n";
  out << "_CORVUS_CXXFLAGS += $(_CORVUS_USER_MACRO_FLAGS)\n";

  // Extra library flags provided by users
  out << "\n";
  out << "# User provided extra library flags\n";
  out << "_CORVUS_USER_LIB_FLAGS ?=\n";
  out << "_CORVUS_LDLIBS += $(_CORVUS_USER_LIB_FLAGS)\n";

  // Verilator runtime libraries
  out << "\n";
  out << "# Verilator runtime libraries (including tracing support)\n";
  out << "_CORVUS_VERILATOR_LIBS = $(_CORVUS_VERILATOR_INCLUDE)/verilated.cpp \\\n";
  out << "                         $(_CORVUS_VERILATOR_INCLUDE)/verilated_threads.cpp \\\n";
  out << "                         $(_CORVUS_VERILATOR_INCLUDE)/verilated_fst_c.cpp\n";
  out << "\n";

  // Extra sources provided by users
  out << "\n";
  out << "# User provided extra sources\n";
  out << "_CORVUS_USER_SRC_FILES ?=\n";

  out << "\n";
  out << "# Generated module object files\n";
  out << "_CORVUS_MODULE_OBJS =";

  // Some GSIM outputs in the repo are "header + cpp + module.json" without a prebuilt lib.
  // For those, we compile the per-module *.cpp into the final binary.
  out << "\n\n";
  out << "# GSIM source files (used when libS*.a is not present)\n";
  out << "_CORVUS_GSIM_SRCS =";

  for (const auto& pair : modules_) {
    const ModuleInfo& m = pair.second;
    std::string var_name = m.module_name;
    std::transform(var_name.begin(), var_name.end(), var_name.begin(), ::toupper);

    const std::string dir_abs = module_path + "/" + (m.is_gsim() ? "gsim-compile-" : "verilator-compile-") + m.module_name;
    std::string lib_expr;
    if (m.is_verilator()) {
      const std::string all_abs = dir_abs + "/" + m.class_name + "__ALL.a";
      const bool has_all = file_exists(all_abs);
      lib_expr = has_all
        ? ("$(_CORVUS_" + var_name + "_DIR)/" + m.class_name + "__ALL.a")
        : ("$(_CORVUS_" + var_name + "_DIR)/lib" + m.class_name + ".a");
    } else {
      // GSIM: prefer lib<class_name>.a when present; otherwise compile *.cpp directly.
      const std::string gsim_lib_abs = dir_abs + "/lib" + m.class_name + ".a";
      const bool has_gsim_lib = file_exists(gsim_lib_abs);
      if (has_gsim_lib) {
        lib_expr = "$(_CORVUS_" + var_name + "_DIR)/lib" + m.class_name + ".a";
      } else {
        // No lib: compile all module-local generated sources.
        out << " \\\n                      $(wildcard $(_CORVUS_" << var_name << "_DIR)/*.cpp)";
        lib_expr.clear();
      }
    }

    if (!lib_expr.empty()) {
      out << " \\\n                      " << lib_expr;
  }
  }

  out << "\n";

  out << "\n\n";
  out << "# Build targets\n";
  out << "# Specify your main source file when running make:\n";
  out << "#   make _CORVUS_TARGET=my_program _CORVUS_MAIN_SRC=my_main.cpp\n";
  out << "_CORVUS_TARGET ?= main\n";
  out << "_CORVUS_MAIN_SRC ?= main.cpp\n";
  out << "_CORVUS_SOURCES = $(_CORVUS_MAIN_SRC) " << wrapper_cpp_file
      << " $(_CORVUS_VERILATOR_LIBS) $(_CORVUS_GSIM_SRCS) $(_CORVUS_USER_SRC_FILES)\n";
  out << "\n";
  out << ".PHONY: _CORVUS_all _CORVUS_clean _CORVUS_test\n";
  out << "\n";
  out << "_CORVUS_all: $(_CORVUS_TARGET)\n";
  out << "\n";
  out << "$(_CORVUS_TARGET): $(_CORVUS_SOURCES) " << wrapper_h_file << "\n";
  out << "\t@echo \"=== Compiling $(_CORVUS_TARGET) ===\"\n";
  out << "\t$(_CORVUS_CXX) $(_CORVUS_CXXFLAGS) -o $@ $(_CORVUS_SOURCES) $(_CORVUS_MODULE_OBJS) $(_CORVUS_LDLIBS)\n";
  out << "\n";
  out << "_CORVUS_test: $(_CORVUS_TARGET)\n";
  out << "\t@echo \"\"\n";
  out << "\t@echo \"=== Running $(_CORVUS_TARGET) ===\"\n";
  out << "\t@./$(_CORVUS_TARGET)\n";
  out << "\n";
  out << "_CORVUS_clean:\n";
  out << "\trm -f $(_CORVUS_TARGET) *.o\n";
  out << "\n";
  out << "_CORVUS_help:\n";
  out << "\t@echo \"Auto-generated Makefile for VCorvusTopWrapper\"\n";
  out << "\t@echo \"=============================================\"\n";
  out << "\t@echo \"Usage:\"\n";
  out << "\t@echo \"  make [_CORVUS_TARGET=name] [_CORVUS_MAIN_SRC=file.cpp]   - Build with custom main file\"\n";
  out << "\t@echo \"  make _CORVUS_test                                        - Build and run test program\"\n";
  out << "\t@echo \"  make _CORVUS_clean                                       - Remove build artifacts\"\n";
  out << "\t@echo \"  make _CORVUS_help                                        - Show this help message\"\n";
  out << "\t@echo \"\"\n";
  out << "\t@echo \"Examples:\"\n";
  out << "\t@echo \"  make                                                     - Build default (test_wrapper)\"\n";
  out << "\t@echo \"  make _CORVUS_TARGET=my_sim _CORVUS_MAIN_SRC=my_sim.cpp   - Build custom program\"\n";

  out.close();

  std::cout << "  Generated Makefile: " << output_makefile << std::endl;
  std::cout << "  Detected " << modules_.size() << " modules" << std::endl;

  return true;
}
