# Coverage Testing Guide

This document provides comprehensive instructions for running code coverage tests for the Chase HTTP Server project.

## Overview

Code coverage measures how much of your source code is executed during testing. The Chase project aims for **>= 80% line coverage** across all core modules.

### Coverage Types

| Type | Description | Target |
|------|-------------|--------|
| Line Coverage | Percentage of source code lines executed | >= 80% |
| Branch Coverage | Percentage of branches taken | >= 70% |
| Function Coverage | Percentage of functions called | >= 90% |

## Prerequisites

### macOS (LLVM Coverage)

macOS uses LLVM's built-in coverage tools (`llvm-profdata` and `llvm-cov`).

```bash
# Install Xcode Command Line Tools (if not installed)
xcode-select --install

# Verify tools are available
which llvm-profdata llvm-cov
```

### Linux (lcov)

Linux uses `lcov` which wraps `gcov`.

```bash
# Ubuntu/Debian
sudo apt-get install lcov

# CentOS/RHEL
sudo yum install lcov

# Arch Linux
sudo pacman -S lcov

# Verify installation
which lcov genhtml
```

## Quick Start

### 1. Build with Coverage Enabled

```bash
# Configure with coverage flags
cmake -B build -DENABLE_COVERAGE=ON

# Build the project
cmake --build build

# Run tests
cd build && ctest --output-on-failure
```

### 2. Generate Coverage Report

```bash
# Generate report (auto-detects platform)
./scripts/coverage_report.sh

# Or specify build directory
./scripts/coverage_report.sh build-debug
```

### 3. View Report

```bash
# macOS: Open in browser
open build/coverage/index.html

# Linux: Open in browser
xdg-open build/coverage/index.html
```

## Detailed Usage

### Script Options

```bash
# Show help
./scripts/coverage_report.sh --help

# Clean old coverage data and regenerate
./scripts/coverage_report.sh --clean

# Verbose output
./scripts/coverage_report.sh --verbose

# Set custom threshold (exit 1 if below)
COVERAGE_THRESHOLD=90 ./scripts/coverage_report.sh

# Specify source directories
SOURCE_DIRS="src,include,internal" ./scripts/coverage_report.sh
```

### Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `COVERAGE_THRESHOLD` | 80 | Minimum coverage percentage |
| `SOURCE_DIRS` | src,include | Comma-separated source directories |
| `VERBOSE` | false | Enable detailed output |

## CMake Configuration

### Build Options

```bash
# Enable coverage
cmake -B build -DENABLE_COVERAGE=ON

# Enable tests (required for coverage)
cmake -B build -DENABLE_TESTS=ON

# Disable examples for faster builds
cmake -B build -DENABLE_EXAMPLES=OFF -DENABLE_COVERAGE=ON
```

### Make Targets

```bash
# Build only tests
cmake --build build --target test_http_parser test_router

# Run specific tests
cd build && ctest -R http_parser -V

# Run all tests with verbose output
cd build && ctest --output-on-failure
```

## Interpreting Coverage Reports

### HTML Report Structure

```
build/coverage/
├── index.html          # Main report entry
├── coverage_text.txt   # Text summary (macOS)
├── coverage.info       # Raw coverage data (Linux)
└── *.html              # Per-file reports
```

### Color Coding

| Color | Meaning |
|-------|---------|
| Green | Line executed (covered) |
| Red | Line not executed (uncovered) |
| Gray | Non-executable line (comments, blank) |
| Orange | Partially covered (branch not taken) |

### Key Metrics

1. **Line Coverage**: Most important metric - aim for >= 80%
2. **Branch Coverage**: Important for complex logic - aim for >= 70%
3. **Function Coverage**: Tracks which functions were called

### Identifying Coverage Gaps

1. Open `build/coverage/index.html`
2. Sort files by coverage percentage (ascending)
3. Focus on files below 80%
4. Click file name to see line-by-line coverage
5. Identify red lines - these are not tested

## Exclusions

The following are automatically excluded from coverage:

```cmake
# Excluded from coverage reports:
# - */test/*           (Test code)
# - */tests/*          (Test code)
# - */examples/*       (Example programs)
# - */build/*          (Build artifacts)
# - */vcpkg_installed/* (Dependencies)
# - *_test.c           (Test files)
```

## CI Integration

### GitHub Actions Workflow

The project includes CI coverage support. See `.github/workflows/ci.yml` for the complete workflow.

```yaml
# Example coverage job (simplified)
coverage:
  runs-on: ubuntu-latest
  steps:
    - uses: actions/checkout@v4
    
    - name: Build with Coverage
      run: |
        cmake -B build -DENABLE_COVERAGE=ON
        cmake --build build
    
    - name: Run Tests
      run: cd build && ctest --output-on-failure
    
    - name: Generate Coverage
      run: ./scripts/coverage_report.sh
    
    - name: Upload Coverage
      uses: actions/upload-artifact@v4
      with:
        name: coverage-report
        path: build/coverage/
```

### Coverage Badges

Add a coverage badge to your README:

```markdown
[![Coverage](https://img.shields.io/badge/coverage-85%25-brightgreen)](./build/coverage/index.html)
```

## Troubleshooting

### No Coverage Data Found

```
Error: No .profraw files found (macOS)
Error: Cannot open coverage file (Linux)
```

**Solution**: Run tests first after building with coverage enabled:
```bash
cmake -B build -DENABLE_COVERAGE=ON
cmake --build build
cd build && ctest
```

### Coverage Tools Not Found

```
Error: llvm-profdata not found (macOS)
Error: lcov not found (Linux)
```

**Solution**: Install required tools:
```bash
# macOS
xcode-select --install

# Linux
sudo apt-get install lcov
```

### Low Coverage Despite Tests

1. Check that tests actually execute the code paths
2. Verify `ENABLE_COVERAGE=ON` was set during configure
3. Clean build directory and rebuild:
   ```bash
   rm -rf build
   cmake -B build -DENABLE_COVERAGE=ON
   cmake --build build
   cd build && ctest
   ./scripts/coverage_report.sh
   ```

### Linker Errors on Linux

```
undefined reference to `__gcov_init'
```

**Solution**: Ensure you're using GCC or Clang with coverage flags:
```bash
# Check compiler
cmake -B build -DENABLE_COVERAGE=ON -DCMAKE_C_COMPILER=gcc
```

### Incompatible Sanitizers

Coverage cannot be used with ThreadSanitizer (`ENABLE_TSAN`):
```bash
# Do NOT combine these
cmake -B build -DENABLE_COVERAGE=ON -DENABLE_TSAN=ON  # WRONG

# Use separately
cmake -B build -DENABLE_COVERAGE=ON    # For coverage
cmake -B build -DENABLE_TSAN=ON        # For race detection
```

## Best Practices

### Writing Testable Code

1. **Keep functions small**: Easier to achieve full coverage
2. **Avoid dead code**: Remove unreachable branches
3. **Separate concerns**: Test each module independently
4. **Use dependency injection**: Makes mocking easier

### Improving Coverage

1. **Start with uncovered files**: Sort by coverage ascending
2. **Focus on critical paths**: Error handling, edge cases
3. **Add boundary tests**: Test min/max values, empty inputs
4. **Test error paths**: Force error conditions
5. **Review test effectiveness**: Ensure tests verify behavior

### Coverage Goals by Module

| Module | Target | Notes |
|--------|--------|-------|
| Core (eventloop, connection) | 85% | Critical paths |
| HTTP (parser, response) | 85% | Protocol handling |
| Security (ssl_wrap, security) | 90% | Security-critical |
| Config (config, vhost) | 80% | Configuration |
| Utilities (buffer, mime) | 75% | Helper functions |

## Advanced Usage

### Generating LCOV-INFO for External Services

```bash
# Linux: Use the generated info file directly
lcov --summary build/coverage.info

# macOS: Convert to lcov format (requires lcov installed)
brew install lcov
# Not directly supported; use llvm-cov export instead
```

### Combining Multiple Test Runs

```bash
# Run tests multiple times to capture different paths
cd build && ctest
# Run specific tests again
./test/test_http_parser
./test/test_router

# Then generate report - all runs are combined
./scripts/coverage_report.sh
```

### Coverage for Specific Files

```bash
# Run only specific tests
cd build && ctest -R http_parser

# Generate report
./scripts/coverage_report.sh
```

## Related Documentation

- [Testing Guide](../README.md#testing) - How to run tests
- [Contributing Guide](../README.md#contributing) - Development workflow
- [CI Configuration](../.github/workflows/ci.yml) - CI pipeline setup