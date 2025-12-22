#include "simulator_interface.h"
#include <dirent.h>
#include <sys/stat.h>
#include <regex>
#include <algorithm>
#include <iostream>

// ============================================================================
// VerilatorSimulator Implementation
// ============================================================================

bool VerilatorSimulator::match_module_directory(const std::string& base_dir,
                                                 const std::string& entry_name,
                                                 std::string& out_module_name) {
  // Pattern: "verilator-compile-<module_name>"
  std::string pattern_str = std::string(DIR_PREFIX) + "(.+)";
  std::regex dir_pattern(pattern_str);
  std::smatch match;

  if (std::regex_match(entry_name, match, dir_pattern)) {
    // Check if it's a directory
    std::string full_path = base_dir + "/" + entry_name;
    struct stat st;
    if (stat(full_path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
      out_module_name = match[1].str();
      return true;
    }
  }
  return false;
}

std::vector<std::string> VerilatorSimulator::discover_modules(const std::string& base_dir) {
  std::vector<std::string> module_names;

  DIR* dir = opendir(base_dir.c_str());
  if (!dir) {
    std::cerr << "Failed to open directory: " << base_dir << std::endl;
    return module_names;
  }

  std::cout << "  [" << get_simulator_name() << "] Scanning directory: " << base_dir << std::endl;

  struct dirent* entry;
  while ((entry = readdir(dir)) != nullptr) {
    std::string module_name;
    if (match_module_directory(base_dir, entry->d_name, module_name)) {
      module_names.push_back(module_name);
      std::cout << "    Found module: " << module_name << std::endl;
    }
  }
  closedir(dir);

  // Sort for consistent ordering
  std::sort(module_names.begin(), module_names.end());

  std::cout << "  Total modules discovered: " << module_names.size() << std::endl;

  return module_names;
}

std::string VerilatorSimulator::get_header_path(const std::string& base_dir,
                                                 const std::string& module_name) {
  // Path pattern: <base_dir>/verilator-compile-<module_name>/V<module_name>.h
  return base_dir + "/" + DIR_PREFIX + module_name + "/" + 
         HEADER_PREFIX + module_name + HEADER_SUFFIX;
}

// ============================================================================
// SimulatorFactory Implementation
// ============================================================================

// ============================================================================
// VCSSimulator Implementation (Placeholder)
// ============================================================================

bool VCSSimulator::match_module_directory(const std::string& base_dir,
                                          const std::string& entry_name,
                                          std::string& out_module_name) {
  // TODO: Implement VCS pattern matching when VCS support is added
  std::string pattern_str = std::string(DIR_PREFIX) + "(.+)";
  std::regex dir_pattern(pattern_str);
  std::smatch match;

  if (std::regex_match(entry_name, match, dir_pattern)) {
    std::string full_path = base_dir + "/" + entry_name;
    struct stat st;
    if (stat(full_path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
      out_module_name = match[1].str();
      return true;
    }
  }
  return false;
}

std::vector<std::string> VCSSimulator::discover_modules(const std::string& base_dir) {
  std::vector<std::string> module_names;

  DIR* dir = opendir(base_dir.c_str());
  if (!dir) {
    std::cerr << "Failed to open directory: " << base_dir << std::endl;
    return module_names;
  }

  std::cout << "  [" << get_simulator_name() << "] Scanning directory: " << base_dir << std::endl;

  struct dirent* entry;
  while ((entry = readdir(dir)) != nullptr) {
    std::string module_name;
    if (match_module_directory(base_dir, entry->d_name, module_name)) {
      module_names.push_back(module_name);
      std::cout << "    Found module: " << module_name << std::endl;
    }
  }
  closedir(dir);

  std::sort(module_names.begin(), module_names.end());
  std::cout << "  Total modules discovered: " << module_names.size() << std::endl;

  return module_names;
}

std::string VCSSimulator::get_header_path(const std::string& base_dir,
                                          const std::string& module_name) {
  // TODO: Implement VCS header path when VCS support is added
  return base_dir + "/" + DIR_PREFIX + module_name + "/" + 
         HEADER_PREFIX + module_name + HEADER_SUFFIX;
}

// ============================================================================
// ModelsimSimulator Implementation (Placeholder)
// ============================================================================

bool ModelsimSimulator::match_module_directory(const std::string& base_dir,
                                                const std::string& entry_name,
                                                std::string& out_module_name) {
  // TODO: Implement Modelsim pattern matching when Modelsim support is added
  std::string pattern_str = std::string(DIR_PREFIX) + "(.+)";
  std::regex dir_pattern(pattern_str);
  std::smatch match;

  if (std::regex_match(entry_name, match, dir_pattern)) {
    std::string full_path = base_dir + "/" + entry_name;
    struct stat st;
    if (stat(full_path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
      out_module_name = match[1].str();
      return true;
    }
  }
  return false;
}

std::vector<std::string> ModelsimSimulator::discover_modules(const std::string& base_dir) {
  std::vector<std::string> module_names;

  DIR* dir = opendir(base_dir.c_str());
  if (!dir) {
    std::cerr << "Failed to open directory: " << base_dir << std::endl;
    return module_names;
  }

  std::cout << "  [" << get_simulator_name() << "] Scanning directory: " << base_dir << std::endl;

  struct dirent* entry;
  while ((entry = readdir(dir)) != nullptr) {
    std::string module_name;
    if (match_module_directory(base_dir, entry->d_name, module_name)) {
      module_names.push_back(module_name);
      std::cout << "    Found module: " << module_name << std::endl;
    }
  }
  closedir(dir);

  std::sort(module_names.begin(), module_names.end());
  std::cout << "  Total modules discovered: " << module_names.size() << std::endl;

  return module_names;
}

std::string ModelsimSimulator::get_header_path(const std::string& base_dir,
                                                const std::string& module_name) {
  // TODO: Implement Modelsim header path when Modelsim support is added
  return base_dir + "/" + DIR_PREFIX + module_name + "/" + 
         HEADER_PREFIX + module_name + HEADER_SUFFIX;
}

// ============================================================================
// GsimSimulator Implementation
// ============================================================================

bool GsimSimulator::match_module_directory(const std::string& base_dir,
                                           const std::string& entry_name,
                                           std::string& out_module_name) {
  // Pattern: "gsim-compile-<module_name>"
  std::string pattern_str = std::string(DIR_PREFIX) + "(.+)";
  std::regex dir_pattern(pattern_str);
  std::smatch match;

  if (std::regex_match(entry_name, match, dir_pattern)) {
    // Check if it's a directory
    std::string full_path = base_dir + "/" + entry_name;
    struct stat st;
    if (stat(full_path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
      out_module_name = match[1].str();
      return true;
    }
  }
  return false;
}

std::vector<std::string> GsimSimulator::discover_modules(const std::string& base_dir) {
  std::vector<std::string> module_names;

  DIR* dir = opendir(base_dir.c_str());
  if (!dir) {
    std::cerr << "Failed to open directory: " << base_dir << std::endl;
    return module_names;
  }

  std::cout << "  [" << get_simulator_name() << "] Scanning directory: " << base_dir << std::endl;

  struct dirent* entry;
  while ((entry = readdir(dir)) != nullptr) {
    std::string module_name;
    if (match_module_directory(base_dir, entry->d_name, module_name)) {
      module_names.push_back(module_name);
      std::cout << "    Found module: " << module_name << std::endl;
    }
  }
  closedir(dir);

  // Sort for consistent ordering
  std::sort(module_names.begin(), module_names.end());
  std::cout << "  Total modules discovered: " << module_names.size() << std::endl;

  return module_names;
}

std::string GsimSimulator::get_header_path(const std::string& base_dir,
                                           const std::string& module_name) {
  // Common GSIM header naming: "<module_name>.h"
  return base_dir + "/" + DIR_PREFIX + module_name + "/" + module_name + ".h";
}

// ============================================================================
// SimulatorFactory Implementation
// ============================================================================

std::unique_ptr<SimulatorInterface> SimulatorFactory::create(SimulatorType type) {
  switch (type) {
    case SimulatorType::VERILATOR:
      return std::unique_ptr<SimulatorInterface>(new VerilatorSimulator());
    
    case SimulatorType::VCS:
      return std::unique_ptr<SimulatorInterface>(new VCSSimulator());
    
    case SimulatorType::MODELSIM:
      return std::unique_ptr<SimulatorInterface>(new ModelsimSimulator());

    case SimulatorType::GSIM:
      return std::unique_ptr<SimulatorInterface>(new GsimSimulator());
    
    default:
      std::cerr << "Unknown simulator type" << std::endl;
      return nullptr;
  }
}

SimulatorFactory::SimulatorType SimulatorFactory::auto_detect(const std::string& base_dir) {
  DIR* dir = opendir(base_dir.c_str());
  if (!dir) {
    std::cerr << "Failed to open directory for auto-detection: " << base_dir << std::endl;
    return SimulatorType::VERILATOR; // Default fallback
  }

  bool has_verilator = false;
  bool has_vcs = false;
  bool has_modelsim = false;
  bool has_gsim = false;

  struct dirent* entry;
  while ((entry = readdir(dir)) != nullptr) {
    std::string entry_name = entry->d_name;
    
    // Check for verilator pattern
    if (entry_name.find("verilator-compile-") == 0) {
      has_verilator = true;
    }
    
    // Check for VCS pattern (example: could be "vcs-compile-" or similar)
    if (entry_name.find("vcs-compile-") == 0) {
      has_vcs = true;
    }
    
    // Check for Modelsim pattern (example: could be "modelsim-compile-" or similar)
    if (entry_name.find("modelsim-compile-") == 0) {
      has_modelsim = true;
    }

    // Check for GSIM pattern
    if (entry_name.find("gsim-compile-") == 0) {
      has_gsim = true;
    }
  }
  closedir(dir);

  // Priority: Verilator > GSIM > VCS > Modelsim
  if (has_verilator) {
    std::cout << "Auto-detected simulator: Verilator" << std::endl;
    return SimulatorType::VERILATOR;
  } else if (has_gsim) {
    std::cout << "Auto-detected simulator: GSIM" << std::endl;
    return SimulatorType::GSIM;
  } else if (has_vcs) {
    std::cout << "Auto-detected simulator: VCS" << std::endl;
    return SimulatorType::VCS;
  } else if (has_modelsim) {
    std::cout << "Auto-detected simulator: Modelsim" << std::endl;
    return SimulatorType::MODELSIM;
  }

  // Default to Verilator if nothing detected
  std::cout << "No simulator pattern detected, defaulting to Verilator" << std::endl;
  return SimulatorType::VERILATOR;
}

std::vector<std::unique_ptr<SimulatorInterface>> SimulatorFactory::create_all() {
  std::vector<std::unique_ptr<SimulatorInterface>> simulators;
  
  // Add Verilator
  simulators.push_back(std::unique_ptr<SimulatorInterface>(new VerilatorSimulator()));

  // Add GSIM
  simulators.push_back(std::unique_ptr<SimulatorInterface>(new GsimSimulator()));
  
  // Add VCS (with placeholder implementation)
  simulators.push_back(std::unique_ptr<SimulatorInterface>(new VCSSimulator()));
  
  // Add Modelsim (with placeholder implementation)
  simulators.push_back(std::unique_ptr<SimulatorInterface>(new ModelsimSimulator()));
  
  return simulators;
}

// ============================================================================
// ModuleDiscoveryManager Implementation
// ============================================================================

ModuleDiscoveryManager::ModuleDiscoveryManager() {
  // Initialize with all available simulators
  simulators_ = SimulatorFactory::create_all();
  std::cout << "ModuleDiscoveryManager initialized with " 
            << simulators_.size() << " simulator(s)" << std::endl;
}

std::vector<ModuleDiscoveryResult> ModuleDiscoveryManager::discover_all_modules(
    const std::string& base_dir) {
  
  std::vector<ModuleDiscoveryResult> results;
  simulator_counts_.clear();

  DIR* dir = opendir(base_dir.c_str());
  if (!dir) {
    std::cerr << "Failed to open directory: " << base_dir << std::endl;
    return results;
  }

  std::cout << "\n=== Mixed-Mode Module Discovery ===" << std::endl;
  std::cout << "  Scanning directory: " << base_dir << std::endl;

  struct dirent* entry;
  while ((entry = readdir(dir)) != nullptr) {
    std::string entry_name = entry->d_name;
    
    // Try each simulator to see which one matches this entry
    for (const auto& simulator : simulators_) {
      std::string module_name;
      if (simulator->match_module_directory(base_dir, entry_name, module_name)) {
        ModuleDiscoveryResult result;
        result.module_name = module_name;
        result.simulator_name = simulator->get_simulator_name();
        result.header_path = simulator->get_header_path(base_dir, module_name);
        
        results.push_back(result);
        simulator_counts_[result.simulator_name]++;
        
        std::cout << "    [" << result.simulator_name << "] Found module: " 
                  << module_name << std::endl;
        break;  // Found a match, no need to try other simulators
      }
    }
  }
  closedir(dir);

  // Sort results by module name for consistent ordering
  std::sort(results.begin(), results.end(), 
            [](const ModuleDiscoveryResult& a, const ModuleDiscoveryResult& b) {
              return a.module_name < b.module_name;
            });

  std::cout << "  Total modules discovered: " << results.size() << std::endl;
  
  return results;
}

void ModuleDiscoveryManager::print_discovery_statistics() const {
  if (simulator_counts_.empty()) {
    std::cout << "  No modules discovered yet." << std::endl;
    return;
  }

  std::cout << "\n=== Discovery Statistics ===" << std::endl;
  for (const auto& pair : simulator_counts_) {
    std::cout << "  " << pair.first << ": " << pair.second << " module(s)" << std::endl;
  }
}
