/**
 * Code Generator
 *
 * Features:
 * 1. Load module information
 * 2. Build connection relationships
 * 3. Generate all propagate function implementations
 * 4. Output statistics
 */

#include "code_generator.h"
#include "cxxopts.hpp"
#include <iostream>
#include <string>
#include <sstream>

static std::string basename_from_path(const std::string& p) {
  size_t pos = p.find_last_of('/');
  return (pos == std::string::npos) ? p : p.substr(pos + 1);
}

int main(int argc, char* argv[]) {
  cxxopts::Options options("Corvusitor", std::string(argv[0]) + ": Generate a wrapper for compiled corvus-compiler output");
  options.add_options()
    ("m,modules-dir", "Path to the modules directory", cxxopts::value<std::string>()->default_value("."))
    ("o,output-name", "Output C++ implementation file name", cxxopts::value<std::string>()->default_value("VCorvusTopWrapper_generated.cpp"))
    ("mode", "Mode: wrapper|diff-single", cxxopts::value<std::string>()->default_value("wrapper"))
    ("diff-module", "Module name for diff-single mode (e.g. corvus_comb_P0)", cxxopts::value<std::string>()->default_value(""))
    ("diff-iters", "Number of random vectors for diff-single mode", cxxopts::value<int>()->default_value("10000"))
    ("diff-seed", "RNG seed for diff-single mode", cxxopts::value<unsigned long long>()->default_value("12648430"))  // 0xC0FFEE
    ("diff-max-width", "Max bitwidth to diff/compare for outputs (inputs are still driven). Default 64 is recommended to start.", cxxopts::value<int>()->default_value("64"))
    ("h,help", "Print usage")
    ;
  auto result = options.parse(argc, argv);
  if (result.count("help")) {
    std::cout << options.help() << std::endl;
    exit(0);
  }

  std::cout << "====================================================\n";
  std::cout << "  Corvusitor\n";
  std::cout << "====================================================\n";

  // Module directory
  std::string modules_dir = result["modules-dir"].as<std::string>();
  std::cout << "\nModule directory: " << modules_dir << "\n";

  std::string mode = result["mode"].as<std::string>();
  if (mode != "wrapper" && mode != "diff-single") {
    std::cerr << "Error: unknown --mode=" << mode << " (expected wrapper|diff-single)\n";
    return 1;
  }

  // Create code generator
  CodeGenerator generator(modules_dir);

  std::string output_cpp = result["output-name"].as<std::string>();
  std::string output_base = output_cpp.substr(0, output_cpp.rfind(".cpp"));
  std::string output_h = output_base + ".h";
  std::string output_mk = output_base + ".mk";

  if (mode == "wrapper") {
    // Load module and connection data
    if (!generator.load_data()) {
      std::cerr << "\nError: Failed to load module data\n";
      return 1;
    }

  std::cout << "\nOutput CPP file: " << output_cpp << "\n";
  std::cout << "Output H file: " << output_h << "\n";

  if (!generator.generate_all(output_base)) {
    std::cerr << "\nError: Failed to generate code\n";
    return 1;
  }

  generator.print_statistics();

  std::cout << "\n====================================================\n";
  std::cout << "  Generation Successful!\n";
  std::cout << "====================================================\n";
  std::cout << "\nNext steps:\n";
  std::cout << "  1. Review generated files:\n";
  std::cout << "     - " << output_cpp << "\n";
  std::cout << "     - " << output_h << "\n";
  std::cout << "     - " << output_mk << "\n";

  // Extract output directory
  std::string output_dir = output_cpp.substr(0, output_cpp.rfind('/') + 1);
  std::cout << "\n  2. Create your main source file (e.g., test_wrapper.cpp) in:\n";
  std::cout << "     " << output_dir << "\n";
  std::cout << "\n  3. Build with auto-generated Makefile:\n";
  std::cout << "     cd " << output_dir << "\n";
  std::cout << "     make -f " << output_mk << "\n";
  std::cout << "\n  4. Or build with custom main file:\n";
  std::cout << "     make -f " << output_mk << " TARGET=my_sim MAIN_SRC=my_sim.cpp\n";
  std::cout << "\n  5. Run 'make -f " << output_mk << " help' for more options\n";
  std::cout << "\n";
    return 0;
  }

  // diff-single mode: generate a standalone diff runner + makefile for one module.
  std::string diff_module = result["diff-module"].as<std::string>();
  int iters = result["diff-iters"].as<int>();
  unsigned long long seed = result["diff-seed"].as<unsigned long long>();
  int max_width = result["diff-max-width"].as<int>();
  if (diff_module.empty()) {
    std::cerr << "Error: --diff-module is required when --mode=diff-single\n";
    return 1;
  }

  std::cout << "\n[diff-single] module=" << diff_module << " iters=" << iters << " seed=" << seed
            << " max_width=" << max_width << "\n";

  if (!generator.generate_single_module_diff(diff_module, output_base, iters, seed, max_width)) {
    std::cerr << "\nError: Failed to generate diff runner\n";
    return 1;
  }

  std::cout << "\n====================================================\n";
  std::cout << "  Diff Runner Generation Successful!\n";
  std::cout << "====================================================\n";
  std::cout << "\nNext steps:\n";
  std::cout << "  cd " << output_cpp.substr(0, output_cpp.rfind('/') + 1) << "\n";
  std::cout << "  make -f " << output_mk << " test\n\n";

  return 0;
}
