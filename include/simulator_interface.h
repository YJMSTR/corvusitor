#ifndef SIMULATOR_INTERFACE_H
#define SIMULATOR_INTERFACE_H

#include "module_info.h"
#include <string>
#include <vector>
#include <memory>
#include <map>

/**
 * ModuleDiscoveryResult - Result of discovering a single module
 */
struct ModuleDiscoveryResult {
  std::string module_name;
  std::string header_path;
  std::string simulator_name;
};

/**
 * SimulatorInterface - Abstract interface for different simulators
 * 
 * This interface provides a unified way to discover and parse modules
 * from different simulator outputs (Verilator, VCS, Modelsim, etc.)
 */
class SimulatorInterface {
public:
  virtual ~SimulatorInterface() = default;

  /**
   * Discover modules in the given directory
   * @param base_dir Base directory containing simulator output
   * @return List of discovered module names
   */
  virtual std::vector<std::string> discover_modules(const std::string& base_dir) = 0;

  /**
   * Check if a directory entry matches this simulator's pattern
   * @param base_dir Base directory
   * @param entry_name Directory entry name
   * @param out_module_name Output: extracted module name if matched
   * @return true if this entry belongs to this simulator
   */
  virtual bool match_module_directory(const std::string& base_dir,
                                       const std::string& entry_name,
                                       std::string& out_module_name) = 0;

  /**
   * Get header file path for a specific module
   * @param base_dir Base directory
   * @param module_name Module name
   * @return Full path to the module's header file
   */
  virtual std::string get_header_path(const std::string& base_dir, 
                                       const std::string& module_name) = 0;

  /**
   * Get the simulator name (for logging/debugging)
   * @return Simulator name string
   */
  virtual std::string get_simulator_name() const = 0;
};

/**
 * VerilatorSimulator - Verilator-specific implementation
 * 
 * Discovers modules in "verilator-compile-<module_name>" directories
 * and locates "V<module_name>.h" header files
 */
class VerilatorSimulator : public SimulatorInterface {
public:
  VerilatorSimulator() = default;
  virtual ~VerilatorSimulator() = default;

  std::vector<std::string> discover_modules(const std::string& base_dir) override;
  
  bool match_module_directory(const std::string& base_dir,
                               const std::string& entry_name,
                               std::string& out_module_name) override;
  
  std::string get_header_path(const std::string& base_dir, 
                               const std::string& module_name) override;
  
  std::string get_simulator_name() const override {
    return "Verilator";
  }

private:
  // Directory pattern: "verilator-compile-<module_name>"
  static constexpr const char* DIR_PREFIX = "verilator-compile-";
  
  // Header file pattern: "V<module_name>.h"
  static constexpr const char* HEADER_PREFIX = "V";
  static constexpr const char* HEADER_SUFFIX = ".h";
};

/**
 * VCSSimulator - VCS-specific implementation (placeholder for future support)
 * 
 * TODO: Implement VCS-specific patterns when needed
 * Example patterns might be: "vcs-compile-<module_name>" or similar
 */
class VCSSimulator : public SimulatorInterface {
public:
  VCSSimulator() = default;
  virtual ~VCSSimulator() = default;

  std::vector<std::string> discover_modules(const std::string& base_dir) override;
  
  bool match_module_directory(const std::string& base_dir,
                               const std::string& entry_name,
                               std::string& out_module_name) override;
  
  std::string get_header_path(const std::string& base_dir, 
                               const std::string& module_name) override;
  
  std::string get_simulator_name() const override {
    return "VCS";
  }

private:
  // TODO: Define VCS-specific patterns
  static constexpr const char* DIR_PREFIX = "vcs-compile-";
  static constexpr const char* HEADER_PREFIX = "";  // TBD
  static constexpr const char* HEADER_SUFFIX = ".h";
};

/**
 * ModelsimSimulator - Modelsim-specific implementation (placeholder for future support)
 * 
 * TODO: Implement Modelsim-specific patterns when needed
 */
class ModelsimSimulator : public SimulatorInterface {
public:
  ModelsimSimulator() = default;
  virtual ~ModelsimSimulator() = default;

  std::vector<std::string> discover_modules(const std::string& base_dir) override;
  
  bool match_module_directory(const std::string& base_dir,
                               const std::string& entry_name,
                               std::string& out_module_name) override;
  
  std::string get_header_path(const std::string& base_dir, 
                               const std::string& module_name) override;
  
  std::string get_simulator_name() const override {
    return "Modelsim";
  }

private:
  // TODO: Define Modelsim-specific patterns
  static constexpr const char* DIR_PREFIX = "modelsim-compile-";
  static constexpr const char* HEADER_PREFIX = "";  // TBD
  static constexpr const char* HEADER_SUFFIX = ".h";
};

/**
 * GsimSimulator - GSIM-specific implementation
 *
 * Discovers modules in "gsim-compile-<module_name>" directories and locates
 * headers/lib based on the module name and/or module.json metadata.
 *
 * Note: GSIM header filename may be either "<module_name>.h" (current emitter)
 * or "<class_name>.h" (legacy/alternate). Corvusitor will prefer module.json
 * when parsing, but discovery still needs a plausible header path.
 */
class GsimSimulator : public SimulatorInterface {
public:
  GsimSimulator() = default;
  virtual ~GsimSimulator() = default;

  std::vector<std::string> discover_modules(const std::string& base_dir) override;

  bool match_module_directory(const std::string& base_dir,
                               const std::string& entry_name,
                               std::string& out_module_name) override;

  std::string get_header_path(const std::string& base_dir,
                               const std::string& module_name) override;

  std::string get_simulator_name() const override {
    return "GSIM";
  }

private:
  // Directory pattern: "gsim-compile-<module_name>"
  static constexpr const char* DIR_PREFIX = "gsim-compile-";
};

/**
 * SimulatorFactory - Factory to create simulator interfaces
 */
class SimulatorFactory {
public:
  enum class SimulatorType {
    VERILATOR,
    VCS,      // For future support
    MODELSIM, // For future support
    GSIM
  };

  /**
   * Create a simulator interface
   * @param type Simulator type
   * @return Unique pointer to simulator interface
   */
  static std::unique_ptr<SimulatorInterface> create(SimulatorType type);

  /**
   * Auto-detect simulator type from directory structure
   * @param base_dir Base directory to inspect
   * @return Detected simulator type
   */
  static SimulatorType auto_detect(const std::string& base_dir);

  /**
   * Create all supported simulators for mixed-mode discovery
   * @return Vector of all available simulator interfaces
   */
  static std::vector<std::unique_ptr<SimulatorInterface>> create_all();
};

/**
 * ModuleDiscoveryManager - Manages module discovery across multiple simulators
 * 
 * This manager can discover modules from a directory that contains outputs
 * from different simulators (e.g., some modules built with Verilator, 
 * others with VCS, etc.)
 */
class ModuleDiscoveryManager {
public:
  ModuleDiscoveryManager();
  
  /**
   * Discover all modules in a directory using all available simulators
   * @param base_dir Base directory containing mixed simulator outputs
   * @return Vector of discovery results, each containing module info and simulator type
   */
  std::vector<ModuleDiscoveryResult> discover_all_modules(const std::string& base_dir);

  /**
   * Get statistics about discovered modules
   */
  void print_discovery_statistics() const;

private:
  std::vector<std::unique_ptr<SimulatorInterface>> simulators_;
  std::map<std::string, int> simulator_counts_;  // Count modules per simulator
};

#endif // SIMULATOR_INTERFACE_H
