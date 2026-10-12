# AGENTS.md

## Authority
- The Chinese version of AGENTS is located at docs/AGENTS_CN.md. Do not modify that file.
- Do not modify any other files in the docs directory.
- Place generated documents in docs-codex.

## Design Requirements
- If a referenced file exists, analyze and follow its instructions first.
- Project requirements are in docs/REQUIREMENTS.md.
- The system architecture design is in docs/ARCHITECTURE.md.
- The detailed design is in docs/DESIGN.md.
- Test requirements are in docs/TEST.md.
- Update the project's implementation plan in docs-codex/DESIGN.md.
- Document the project's latest build instructions and usage in readme.md.

## Comment Requirements
- Add comments only when they do not affect code execution.

## Commit Requirements
- Do not incorrectly attribute an entire human-written file or function to Codex.
- Preserve existing copyright, license, and other valid attribution notices.
- Check .gitignore, and do not add intermediate files or build artifacts to Git.

## Test Requirements
- Place the test report for the current version in docs-codex/test-report.md, and remove test content related to the previous version.

## C Coding Guidelines

See [.clang-format](../.clang-format) for automatic formatting rules. This document lists only additional conventions that require manual compliance.

### Files and Organization
- Use UTF-8 for source code. Use lowercase file and directory names, preferably separating words with underscores. Names should describe their purpose.
- Header files must have include guards using the existing `__<FILE>_H_` convention. Include only declarations, type definitions, and similar content; do not include function bodies.
- Organize header files in this order: file comment, preprocessing section, then type and function declarations. Organize source files in this order: file comment, header includes, then data and function implementations.
- Order header includes as follows: the corresponding header, C standard library headers, other library headers, then internal project headers. Follow project conventions for ordering within each category.

### Additional Manual Formatting Requirements
- Separate relatively independent functions and code blocks such as `if/switch/for/do/while` from other code with blank lines before and after them.
- Place standalone explanatory comments immediately above the corresponding code, separated from preceding code by a blank line. Do not leave a blank line between a comment and the code it describes. Comments for individual statements or structure fields may also appear to their right, but not below them.
- `#include` and `#define` directives are exempt from the column limit. When manual macro formatting must be preserved, use `clang-format off/on` only around the necessary section. Check excessively long identifiers and similar cases manually.

### Naming
- Use PascalCase for types, such as `SensorData`. Use lowercase names with underscores for variables, parameters, and structure members, such as `sample_count`.
- Use `k` followed by PascalCase for constants, such as `kDaysInAWeek`. Use uppercase names with underscores for macros and enumeration values.
- Use mixed-case names for ordinary functions. The original guidelines do not specify the case of the initial letter; these guidelines standardize on PascalCase, such as `ReadSensor`. Getter and setter names should match the variable name, such as `get_count` and `set_count`.

### Comments
- Meaningful comment lines must account for at least 30% of all code lines. Blank lines, empty comments, and unfilled template lines do not count as meaningful comments. Chinese comments are recommended.
- At the beginning of each file, document the file name, person responsible, creation date, purpose, change log, copyright, and legal information. Enter “None” where no applicable information is available.
  ```c
  /**
   * @file File name
   * @author [Original author],Codex
   * @brief Brief description of functionality
   * @date Creation date
   * @copyright Copyright (c) Year
   */
  ```
- Use the following template for function comments:
  - Document necessary input/output requirements, null-pointer constraints, memory ownership, performance requirements, and synchronization prerequisites in the appropriate fields. Add necessary implementation details at the function definition.
    ```c
    /**
     * @authors [Original author],Codex
     * @brief Brief description of functionality
     * @details (Optional) Detailed description
     * @param parameter_name Parameter description. Use one `@param parameter_name description` entry per parameter; omit `@param` when there are no parameters.
     * @return Return value description. Use `@return None` when there is no return value or the return type is `void`.
     * @note (Optional) Additional notes
     */
    ```
- Comment on data structures that are not self-explanatory; variables, constants, and macros with physical meaning; and complex, critical, or non-obvious implementations. Describe the meaning and units of physical quantities.
- When modifying code, explain how and why it was changed. Keep only the latest modification note at each location. Use `TODO` for temporary code or code that needs further work.
- Add comments to generated code for each functional block.

### Macros, Variables, and Expressions
- Fully parenthesize each parameter and the entire expression in expression macros. Enclose multi-statement macros in braces; `do { ... } while (0)` is recommended.
- Initialize basic types and pointers when defining them. Initialize arrays, structures, and unions when defining them or immediately afterward.
- Local variables must not share names with global variables. Remove unused global variables and prefer local variables. Use `static` for module-private global variables, and use `extern` only when necessary.
- Except for simple assignments, use parentheses to make the evaluation order of compound expressions explicit. Do not use numbers with unclear meanings. Use meaningful macros or enumerations for physical states and quantities; when a numeric literal must be used directly, explain its meaning.

### Control Statements and Functions
- Do not compare floating-point values directly with numbers using `==` or `!=`. Use tolerance or range comparisons based on the precision required by the application. Use `NULL` for pointer null checks. Placing constants first in conditional comparisons is recommended.
- Do not modify loop variables inside a `for` loop body. Every `switch` must have a `default` branch that performs actual handling; if it must be omitted, explain why.
- Specify the type and name of each function parameter. Using `void` for functions with no parameters is recommended. Place input parameters before output parameters. Pass structure parameters by pointer.
- Keep functions short and give them clear responsibilities. Reentrant functions that use shared state or `static` local variables must handle synchronization.
- Interrupt handlers must not call time-consuming or blocking functions, or functions unsuitable for interrupt context, such as printing, blocking waits, or dynamic memory allocation.

### Memory and Maintenance
- Check for `NULL` immediately after allocating dynamic memory. After a successful allocation, initialize the memory before reading it. Ensure that memory is correctly released on every execution path.
- Set the corresponding pointer to `NULL` immediately after `free`, and prevent other references from continuing to access the freed memory.
- A consistent debug output format that includes the module or file name and line number is recommended. Address compiler warnings. When modifying existing code, maintain consistency with the surrounding style. Existing noncompliant code may be cleaned up gradually according to project conventions.
