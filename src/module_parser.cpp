#include "module_parser.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <cctype>
#include <sys/stat.h>

static bool file_exists(const std::string& path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// Normalize port names across simulators so ConnectionBuilder can match them.
// Verilator sometimes encodes '/' as "2F" in C++ member identifiers; GSIM typically uses '_'.
// We keep the original identifier in PortInfo.cpp_name and use the normalized name for PortInfo.name.
static std::string normalize_port_logical_name(std::string n) {
  for (size_t pos = 0; (pos = n.find("2F", pos)) != std::string::npos;) {
    n.replace(pos, 2, "_");
    pos += 1;
  }
  return n;
}

// ============================================================================
// ModuleParser Base Class - Common utility functions
// ============================================================================

ModuleType ModuleParser::infer_module_type(const std::string& module_name) {
  if (module_name.find("_comb_") != std::string::npos) {
    return ModuleType::COMB;
  } else if (module_name.find("_seq_") != std::string::npos) {
    return ModuleType::SEQ;
  } else if (module_name.find("_external") != std::string::npos) {
    return ModuleType::EXTERNAL;
  }
  throw std::runtime_error("Unknown module type: " + module_name);
}

int ModuleParser::extract_partition_id(const std::string& module_name) {
  // Find the number after _P
  size_t p_pos = module_name.find("_P");
  if (p_pos == std::string::npos) {
    return -1;  // external has no partition ID
  }

  // Extract the number after P
  std::string id_str;
  for (size_t i = p_pos + 2; i < module_name.length(); i++) {
    if (std::isdigit(module_name[i])) {
      id_str += module_name[i];
    } else {
      break;
    }
  }

  if (id_str.empty()) {
    return -1;
  }

  return std::stoi(id_str);
}

std::string ModuleParser::generate_instance_name(const std::string& class_name, ModuleType /* type */) {
  // "Vcorvus_comb_P0" -> "comb_p0"
  // "Vcorvus_seq_P1" -> "seq_p1"
  // "Vcorvus_external" -> "external"

  std::string name = class_name;

  // Remove V prefix
  if (name[0] == 'V') {
    name = name.substr(1);
  }

  // Remove corvus_ prefix
  size_t corvus_pos = name.find("corvus_");
  if (corvus_pos == 0) {
    name = name.substr(7);  // "corvus_" length is 7
  }

  // Convert to lowercase and handle P -> p
  for (size_t i = 0; i < name.length(); i++) {
    if (name[i] == 'P' && i + 1 < name.length() && std::isdigit(name[i + 1])) {
      name[i] = 'p';
    } else {
      name[i] = std::tolower(name[i]);
    }
  }

  return name;
}

// ============================================================================
// VerilatorModuleParser Implementation
// ============================================================================

VerilatorModuleParser::VerilatorModuleParser() {
  // Match VL_IN8(&signal_name, 7, 0); or VL_OUT64(&signal_name, 63, 0);
  // Note: 17-32 bits use VL_IN/VL_OUT (no suffix), not VL_IN32/VL_OUT32
  // Note: 65+ bits use VL_INW/OUTW with 4 arguments: (&name, msb, lsb, words)
  // 3-argument format: VL_(IN|OUT)(8|16|64|)(&name, msb, lsb)
  port_macro_regex = std::regex(
    R"(VL_(IN|OUT)(8|16|64|)\s*\(\s*&([^,\)]+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\))"
  );
  // 4-argument format: VL_(IN|OUT)W(&name, msb, lsb, words)
  port_macro_regex_wide = std::regex(
    R"(VL_(IN|OUT)W\s*\(\s*&([^,\)]+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\))"
  );
}

ModuleInfo VerilatorModuleParser::parse(const std::string& header_path) {
  ModuleInfo info;
  info.header_path = header_path;
  info.simulator_name = get_simulator_name();

  // Extract module name from file name
  // "/path/to/Vcorvus_comb_P0.h" -> "Vcorvus_comb_P0"
  size_t last_slash = header_path.find_last_of('/');
  size_t dot_pos = header_path.find_last_of('.');

  if (last_slash == std::string::npos) {
    last_slash = 0;
  } else {
    last_slash++;
  }

  if (dot_pos == std::string::npos || dot_pos < last_slash) {
    throw std::runtime_error("Invalid header file path: " + header_path);
  }

  info.class_name = header_path.substr(last_slash, dot_pos - last_slash);

  // Remove 'V' prefix to get module name
  if (info.class_name[0] == 'V') {
    info.module_name = info.class_name.substr(1);
  } else {
    info.module_name = info.class_name;
  }

  // Infer module type and partition ID
  info.type = infer_module_type(info.module_name);
  info.partition_id = extract_partition_id(info.module_name);
  info.instance_name = generate_instance_name(info.class_name, info.type);
  info.lib_path = infer_lib_path(header_path, info.class_name);

  // Read file and parse ports
  std::ifstream file(header_path);
  if (!file.is_open()) {
    throw std::runtime_error("Failed to open: " + header_path);
  }

  std::string line;
  int line_num = 0;
  while (std::getline(file, line)) {
    line_num++;
    // Find VL_IN/VL_OUT macro
    if (line.find("VL_IN") != std::string::npos ||
      line.find("VL_OUT") != std::string::npos) {
      try {
        PortInfo port = parse_port_macro(line);
        info.ports.push_back(port);
      } catch (const std::exception& e) {
        // Parse failure is not fatal, just a warning
        std::cerr << "Warning at line " << line_num << ": " << e.what() << std::endl;
      }
    }
  }

  std::cout << "Parsed " << info.ports.size() << " ports from " << info.class_name << std::endl;

  return info;
}

PortInfo VerilatorModuleParser::parse_port_macro(const std::string& line) {
  std::smatch match;
  PortInfo port;

  // Try matching 4-argument format (VL_INW/OUTW)
  if (std::regex_search(line, match, port_macro_regex_wide)) {
    // Direction: IN or OUT
    port.direction = (match[1] == "IN") ? PortDirection::INPUT : PortDirection::OUTPUT;

    // Bit width type: W (65+ bits)
    port.width_type = PortWidthType::VL_W;

    // Port name
    port.name = match[2];
    port.name.erase(0, port.name.find_first_not_of(" \t"));
    port.name.erase(port.name.find_last_not_of(" \t") + 1);
    port.cpp_name = port.name;
    port.name = normalize_port_logical_name(port.name);

    // msb, lsb, words
    port.msb = std::stoi(match[3]);
    port.lsb = std::stoi(match[4]);
    port.array_size = std::stoi(match[5]);  // words 参数

    return port;
  }

  // Then try matching 3-argument format (VL_IN8/16/64/etc)
  if (std::regex_search(line, match, port_macro_regex)) {
    // Direction: IN or OUT
    port.direction = (match[1] == "IN") ? PortDirection::INPUT : PortDirection::OUTPUT;

    // Bit width type: 8, 16, "", 64
    // Note: empty string "" means 17-32 bits (VL_IN/VL_OUT no suffix)
    std::string width_str = match[2];
    if (width_str == "8")       port.width_type = PortWidthType::VL_8;
    else if (width_str == "16") port.width_type = PortWidthType::VL_16;
    else if (width_str == "")   port.width_type = PortWidthType::VL_32;  // VL_IN/VL_OUT (17-32位)
    else if (width_str == "64") port.width_type = PortWidthType::VL_64;
    else throw std::runtime_error("Unknown width type: " + width_str);

    // Port name
    port.name = match[3];
    port.name.erase(0, port.name.find_first_not_of(" \t"));
    port.name.erase(port.name.find_last_not_of(" \t") + 1);
    port.cpp_name = port.name;
    port.name = normalize_port_logical_name(port.name);

    // msb, lsb
    port.msb = std::stoi(match[4]);
    port.lsb = std::stoi(match[5]);
    port.array_size = 0;  // Not an array type

    return port;
  }

  throw std::runtime_error("Invalid port macro format");
}

std::string VerilatorModuleParser::infer_lib_path(const std::string& header_path, const std::string& class_name) {
  // "/path/to/verilator-compile-xxx/Vxxx.h"
  // -> "/path/to/verilator-compile-xxx/libVxxx.a"

  size_t last_slash = header_path.find_last_of('/');
  std::string dir;

  if (last_slash != std::string::npos) {
    dir = header_path.substr(0, last_slash + 1);
  } else {
    dir = "./";
  }

  return dir + "lib" + class_name + ".a";
}

// ============================================================================
// VCSModuleParser Implementation (Placeholder)
// ============================================================================

VCSModuleParser::VCSModuleParser() {
  // TODO: Initialize VCS-specific regex patterns
}

ModuleInfo VCSModuleParser::parse(const std::string& header_path) {
  // TODO: Implement VCS-specific parsing logic
  std::cerr << "VCS parser not yet implemented for: " << header_path << std::endl;
  throw std::runtime_error("VCS parser not yet implemented");
}

// ============================================================================
// ModelsimModuleParser Implementation (Placeholder)
// ============================================================================

ModelsimModuleParser::ModelsimModuleParser() {
  // TODO: Initialize Modelsim-specific regex patterns
}

ModuleInfo ModelsimModuleParser::parse(const std::string& header_path) {
  // TODO: Implement Modelsim-specific parsing logic
  std::cerr << "Modelsim parser not yet implemented for: " << header_path << std::endl;
  throw std::runtime_error("Modelsim parser not yet implemented");
}

// ============================================================================
// GsimModuleParser Implementation
// ============================================================================

ModuleType GsimModuleParser::parse_module_type_str(const std::string& s) {
  if (s == "COMB") return ModuleType::COMB;
  if (s == "SEQ") return ModuleType::SEQ;
  if (s == "EXTERNAL") return ModuleType::EXTERNAL;
  throw std::runtime_error("Unknown GSIM module type: " + s);
}

ModuleInfo GsimModuleParser::parse(const std::string& header_path) {
  ModuleInfo info;
  info.header_path = header_path;
  info.simulator_name = get_simulator_name();

  // Locate module directory and module.json
  size_t last_slash = header_path.find_last_of('/');
  std::string dir = (last_slash == std::string::npos) ? "./" : header_path.substr(0, last_slash);
  std::string json_path = dir + "/module.json";

  std::ifstream jf(json_path);
  if (!jf.is_open()) {
    throw std::runtime_error("Failed to open GSIM module.json: " + json_path);
  }

  std::stringstream buffer;
  buffer << jf.rdbuf();
  std::string json = buffer.str();

  // Minimal regex-based parsing (module.json is a small, regular format in this project).
  auto extract_str = [&](const std::string& key) -> std::string {
    std::regex re("\"" + key + "\"\\s*:\\s*\"([^\"]+)\"");
    std::smatch m;
    if (!std::regex_search(json, m, re)) {
      throw std::runtime_error("Missing key in module.json: " + key);
    }
    return m[1].str();
  };
  auto extract_int = [&](const std::string& key) -> int {
    std::regex re("\"" + key + "\"\\s*:\\s*(-?\\d+)");
    std::smatch m;
    if (!std::regex_search(json, m, re)) {
      throw std::runtime_error("Missing key in module.json: " + key);
    }
    return std::stoi(m[1].str());
  };

  info.module_name = extract_str("module_name");
  info.class_name = extract_str("class_name");
  info.type = parse_module_type_str(extract_str("type"));
  info.partition_id = extract_int("partition_id");

  // Instance name: follow existing convention (lowercase, strip corvus_ when present).
  info.instance_name = generate_instance_name(info.class_name, info.type);

  // Header path selection: try class_name.h then module_name.h, then fallback to the provided header_path.
  std::string candidate1 = dir + "/" + info.class_name + ".h";
  std::string candidate2 = dir + "/" + info.module_name + ".h";
  if (file_exists(candidate1)) info.header_path = candidate1;
  else if (file_exists(candidate2)) info.header_path = candidate2;
  else info.header_path = header_path;

  // Library path selection: try lib<class_name>.a then libS<module_name>.a then lib<module_name>.a
  std::string lib1 = dir + "/lib" + info.class_name + ".a";
  std::string lib2 = dir + "/libS" + info.module_name + ".a";
  std::string lib3 = dir + "/lib" + info.module_name + ".a";
  if (file_exists(lib1)) info.lib_path = lib1;
  else if (file_exists(lib2)) info.lib_path = lib2;
  else if (file_exists(lib3)) info.lib_path = lib3;
  else info.lib_path = lib1; // default expected name

  // Parse ports: {"name": "...", "direction": "input|output", "width": N, ...}
  // Note: keep it simple; we only need name/direction/width for the integration plan.
  std::regex port_re("\\{[^\\}]*\"name\"\\s*:\\s*\"([^\"]+)\"[^\\}]*\"direction\"\\s*:\\s*\"(input|output)\"[^\\}]*\"width\"\\s*:\\s*(\\d+)[^\\}]*\\}");
  auto begin = std::sregex_iterator(json.begin(), json.end(), port_re);
  auto end = std::sregex_iterator();
  for (auto it = begin; it != end; ++it) {
    std::smatch m = *it;
    PortInfo p;
    p.name = m[1].str();
    p.cpp_name = p.name;
    p.name = normalize_port_logical_name(p.name);
    std::string dir_str = m[2].str();
    int width = std::stoi(m[3].str());
    if (width <= 0) width = 1;  // be defensive: treat unknown/0-width as 1-bit

    p.direction = (dir_str == "input") ? PortDirection::INPUT : PortDirection::OUTPUT;
    p.lsb = 0;
    p.msb = width > 0 ? (width - 1) : 0;
    p.array_size = 0;

    if (width <= 8) p.width_type = PortWidthType::VL_8;
    else if (width <= 16) p.width_type = PortWidthType::VL_16;
    else if (width <= 32) p.width_type = PortWidthType::VL_32;
    else if (width <= 64) p.width_type = PortWidthType::VL_64;
    else {
      // Placeholder mapping for wide ports; real GSIM uses _BitInt, Verilator uses VlWide.
      // Keep something non-crashing for downstream code that expects a width_type.
      p.width_type = PortWidthType::VL_W;
      p.array_size = (width + 31) / 32;
    }

    info.ports.push_back(p);
  }

  if (info.ports.empty()) {
    throw std::runtime_error("No ports parsed from GSIM module.json: " + json_path);
  }

  std::cout << "Parsed " << info.ports.size() << " ports from GSIM module.json (" << info.module_name << ")\n";
  return info;
}

// ============================================================================
// ModuleParserFactory Implementation
// ============================================================================

std::unique_ptr<ModuleParser> ModuleParserFactory::create(const std::string& simulator_name) {
  if (simulator_name == "Verilator") {
    return std::unique_ptr<ModuleParser>(new VerilatorModuleParser());
  } else if (simulator_name == "GSIM") {
    return std::unique_ptr<ModuleParser>(new GsimModuleParser());
  } else if (simulator_name == "VCS") {
    return std::unique_ptr<ModuleParser>(new VCSModuleParser());
  } else if (simulator_name == "Modelsim") {
    return std::unique_ptr<ModuleParser>(new ModelsimModuleParser());
  } else {
    std::cerr << "Unknown simulator: " << simulator_name << std::endl;
    return nullptr;
  }
}
