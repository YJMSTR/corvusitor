#ifndef MODULE_INFO_H
#define MODULE_INFO_H

#include "port_info.h"
#include <vector>
#include <string>

enum class ModuleType {
  COMB,
  SEQ,
  EXTERNAL
};

struct ModuleInfo {
  std::string module_name;    // "corvus_comb_P0"
  std::string class_name;     // "Vcorvus_comb_P0"
  std::string instance_name;  // "comb_p0"
  ModuleType type;
  int partition_id;           // P0 -> 0, P1 -> 1, external -> -1

  // Which simulator backend produced this module (e.g. "Verilator", "GSIM").
  // Used by CodeGenerator to choose eval/step and port access style.
  std::string simulator_name;

  std::string header_path;
  std::string lib_path;

  std::vector<PortInfo> ports;

  // Get input ports
  std::vector<PortInfo> get_inputs() const {
    std::vector<PortInfo> inputs;
    for (const auto& port : ports) {
      if (port.direction == PortDirection::INPUT) {
        inputs.push_back(port);
      }
    }
    return inputs;
  }

  // Get output ports
  std::vector<PortInfo> get_outputs() const {
    std::vector<PortInfo> outputs;
    for (const auto& port : ports) {
      if (port.direction == PortDirection::OUTPUT) {
        outputs.push_back(port);
      }
    }
    return outputs;
  }

  // Get module type string
  std::string get_type_str() const {
    switch (type) {
      case ModuleType::COMB: return "COMB";
      case ModuleType::SEQ: return "SEQ";
      case ModuleType::EXTERNAL: return "EXTERNAL";
      default: return "UNKNOWN";
    }
  }

  bool is_gsim() const { return simulator_name == "GSIM"; }
  bool is_verilator() const { return simulator_name == "Verilator"; }
};

#endif // MODULE_INFO_H
