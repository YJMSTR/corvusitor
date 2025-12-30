#ifndef PORT_INFO_H
#define PORT_INFO_H

#include <string>

enum class PortDirection {
  INPUT,
  OUTPUT
};

enum class PortWidthType {
  VL_8,   // CData: uint8_t
  VL_16,  // SData: uint16_t
  VL_32,  // IData: uint32_t
  VL_64,  // QData: uint64_t
  VL_W    // WData: arbitrary width array
};

struct PortInfo {
  // Logical port name used for cross-module matching and top-level wrapper interface.
  // This may be normalized (e.g. "2F" -> "_" for Verilator-encoded hierarchical names).
  std::string name;
  // C++ identifier used to access the port in the generated simulator C++ API.
  // For Verilator this is typically the original member name (may contain "2F").
  // For GSIM this is typically the method suffix / member name from module.json.
  std::string cpp_name;
  PortDirection direction;
  PortWidthType width_type;
  int msb;
  int lsb;
  int array_size;  // For VL_W: number of words in VlWide<array_size>

  // Get actual bit width
  int get_width() const {
    return msb - lsb + 1;
  }

  // Get the C++ identifier to use in generated code.
  // Falls back to logical name when cpp_name is not populated (backward-compatible).
  std::string get_cpp_name() const {
    return cpp_name.empty() ? name : cpp_name;
  }

  // Get corresponding C++ type
  std::string get_cpp_type() const {
    switch (width_type) {
      case PortWidthType::VL_8:  return "CData";
      case PortWidthType::VL_16: return "SData";
      case PortWidthType::VL_32: return "IData";
      case PortWidthType::VL_64: return "QData";
      case PortWidthType::VL_W:
        return "VlWide<" + std::to_string(array_size) + ">";
      default: return "unknown";
    }
  }

  // Get direction string
  std::string get_direction_str() const {
    return (direction == PortDirection::INPUT) ? "INPUT" : "OUTPUT";
  }
};

#endif // PORT_INFO_H
