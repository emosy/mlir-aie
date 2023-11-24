#!/usr/bin/env python3
"""A script to generate FileCheck statements for mlir unit tests.

This script is a utility to add FileCheck patterns to an mlir file.

NOTE: The input .mlir is expected to be the output from the parser, not a
stripped down variant.

Example usage:
$ generate-test-checks.py foo.mlir
$ mlir-opt foo.mlir -transformation | generate-test-checks.py
$ mlir-opt foo.mlir -transformation | generate-test-checks.py --source foo.mlir
$ mlir-opt foo.mlir -transformation | generate-test-checks.py --source foo.mlir -i
$ mlir-opt foo.mlir -transformation | generate-test-checks.py --source foo.mlir -i --source_delim_regex='gpu.func @'

The script will heuristically generate CHECK/CHECK-LABEL commands for each line
within the file. By default this script will also try to insert string
substitution blocks for all SSA value names. If --source file is specified, the
script will attempt to insert the generated CHECKs to the source file by looking
for line positions matched by --source_delim_regex.

The script is designed to make adding checks to a test case fast, it is *not*
designed to be authoritative about what constitutes a good test!
"""

# Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import argparse
import os  # Used to advertise this file's name ("autogenerated_note").
import re
import sys

ADVERT_BEGIN = "// NOTE: Assertions have been autogenerated by "
ADVERT_END = """
// The script is designed to make adding checks to
// a test case fast, it is *not* designed to be authoritative
// about what constitutes a good test! The CHECK should be
// minimized and named to reflect the test intent.
"""


# Regex command to match an SSA identifier.
SSA_RE_STR = "[0-9]+|[a-zA-Z$._-][a-zA-Z0-9$._-]*"
SSA_RE = re.compile(SSA_RE_STR)

# Regex matching the left-hand side of an assignment
SSA_RESULTS_STR = r'\s*(%' + SSA_RE_STR + r')(\s*,\s*(%' + SSA_RE_STR + r'))*\s*='
SSA_RESULTS_RE = re.compile(SSA_RESULTS_STR)

# Regex matching attributes
ATTR_RE_STR = r'(#[a-zA-Z._-][a-zA-Z0-9._-]*)'
ATTR_RE = re.compile(ATTR_RE_STR)

# Regex matching the left-hand side of an attribute definition
ATTR_DEF_RE_STR = r'\s*' + ATTR_RE_STR + r'\s*='
ATTR_DEF_RE = re.compile(ATTR_DEF_RE_STR)


# Class used to generate and manage string substitution blocks for SSA value
# names.
class VariableNamer:
    def __init__(self, variable_names):
        self.scopes = []
        self.name_counter = 0

        # Number of variable names to still generate in parent scope
        self.generate_in_parent_scope_left = 0

        # Parse variable names
        self.variable_names = [name.upper() for name in variable_names.split(',')]
        self.used_variable_names = set()

    # Generate the following 'n' variable names in the parent scope.
    def generate_in_parent_scope(self, n):
        self.generate_in_parent_scope_left = n

    # Generate a substitution name for the given ssa value name.
    def generate_name(self, source_variable_name):

        # Compute variable name
        variable_name = self.variable_names.pop(0) if len(self.variable_names) > 0 else ''
        if variable_name == '':
            variable_name = "VAL_" + str(self.name_counter)
            self.name_counter += 1

        # Scope where variable name is saved
        scope = len(self.scopes) - 1
        if self.generate_in_parent_scope_left > 0:
            self.generate_in_parent_scope_left -= 1
            scope = len(self.scopes) - 2
        assert(scope >= 0)

        # Save variable
        if variable_name in self.used_variable_names:
            raise RuntimeError(variable_name + ': duplicate variable name')
        self.scopes[scope][source_variable_name] = variable_name
        self.used_variable_names.add(variable_name)

        return variable_name

    # Push a new variable name scope.
    def push_name_scope(self):
        self.scopes.append({})

    # Pop the last variable name scope.
    def pop_name_scope(self):
        self.scopes.pop()

    # Return the level of nesting (number of pushed scopes).
    def num_scopes(self):
        return len(self.scopes)

    # Reset the counter and used variable names.
    def clear_names(self):
        self.name_counter = 0
        self.used_variable_names = set()

class AttributeNamer:

    def __init__(self, attribute_names):
        self.name_counter = 0
        self.attribute_names = [name.upper() for name in attribute_names.split(',')]
        self.map = {}
        self.used_attribute_names = set()

    # Generate a substitution name for the given attribute name.
    def generate_name(self, source_attribute_name):

        # Compute FileCheck name
        attribute_name = self.attribute_names.pop(0) if len(self.attribute_names) > 0 else ''
        if attribute_name == '':
            attribute_name = "ATTR_" + str(self.name_counter)
            self.name_counter += 1

        # Prepend global symbol
        attribute_name = '$' + attribute_name

        # Save attribute
        if attribute_name in self.used_attribute_names:
            raise RuntimeError(attribute_name + ': duplicate attribute name')
        self.map[source_attribute_name] = attribute_name
        self.used_attribute_names.add(attribute_name)
        return attribute_name

    # Get the saved substitution name for the given attribute name. If no name
    # has been generated for the given attribute yet, the source attribute name
    # itself is returned.
    def get_name(self, source_attribute_name):
        return self.map[source_attribute_name] if source_attribute_name in self.map else '?'

# Return the number of SSA results in a line of type
#   %0, %1, ... = ...
# The function returns 0 if there are no results.
def get_num_ssa_results(input_line):
    m = SSA_RESULTS_RE.match(input_line)
    return m.group().count('%') if m else 0


# Process a line of input that has been split at each SSA identifier '%'.
def process_line(line_chunks, variable_namer):
    output_line = ""

    # Process the rest that contained an SSA value name.
    for chunk in line_chunks:
        m = SSA_RE.match(chunk)
        ssa_name = m.group(0) if m is not None else ''

        # Check if an existing variable exists for this name.
        variable = None
        for scope in variable_namer.scopes:
            variable = scope.get(ssa_name)
            if variable is not None:
                break

        # If one exists, then output the existing name.
        if variable is not None:
            output_line += "%[[" + variable + "]]"
        else:
            # Otherwise, generate a new variable.
            variable = variable_namer.generate_name(ssa_name)
            output_line += "%[[" + variable + ":.*]]"

        # Append the non named group.
        output_line += chunk[len(ssa_name) :]

    return output_line.rstrip() + "\n"


# Process the source file lines. The source file doesn't have to be .mlir.
def process_source_lines(source_lines, note, args):
    source_split_re = re.compile(args.source_delim_regex)

    source_segments = [[]]
    for line in source_lines:
        # Remove previous note.
        if line == note:
            continue
        # Remove previous CHECK lines.
        if line.find(args.check_prefix) != -1:
            continue
        # Segment the file based on --source_delim_regex.
        if source_split_re.search(line):
            source_segments.append([])

        source_segments[-1].append(line + "\n")
    return source_segments

def process_attribute_definition(line, attribute_namer, output):
    m = ATTR_DEF_RE.match(line)
    if m:
        attribute_name = attribute_namer.generate_name(m.group(1))
        line = '// CHECK: #[[' + attribute_name + ':.+]] =' + line[len(m.group(0)):] + '\n'
        output.write(line)

def process_attribute_references(line, attribute_namer):

    output_line = ''
    components = ATTR_RE.split(line)
    for component in components:
        m = ATTR_RE.match(component)
        if m:
            output_line += '#[[' + attribute_namer.get_name(m.group(1)) + ']]'
            output_line += component[len(m.group()):]
        else:
            output_line += component
    return output_line

# Pre-process a line of input to remove any character sequences that will be
# problematic with FileCheck.
def preprocess_line(line):
    # Replace any double brackets, '[[' with escaped replacements. '[['
    # corresponds to variable names in FileCheck.
    output_line = line.replace("[[", "{{\\[\\[}}")

    # Replace any single brackets that are followed by an SSA identifier, the
    # identifier will be replace by a variable; Creating the same situation as
    # above.
    output_line = output_line.replace("[%", "{{\\[}}%")

    return output_line


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawTextHelpFormatter
    )
    parser.add_argument(
        "--check-prefix", default="CHECK", help="Prefix to use from check file."
    )
    parser.add_argument(
        "-o", "--output", nargs="?", type=argparse.FileType("w"), default=None
    )
    parser.add_argument(
        "input", nargs="?", type=argparse.FileType("r"), default=sys.stdin
    )
    parser.add_argument(
        "--source",
        type=str,
        help="Print each CHECK chunk before each delimeter line in the source"
        "file, respectively. The delimeter lines are identified by "
        "--source_delim_regex.",
    )
    parser.add_argument("--source_delim_regex", type=str, default="func @")
    parser.add_argument(
        "--starts_from_scope",
        type=int,
        default=1,
        help="Omit the top specified level of content. For example, by default "
        'it omits "module {"',
    )
    parser.add_argument("-i", "--inplace", action="store_true", default=False)
    parser.add_argument(
        "--variable_names",
        type=str,
        default='',
        help="Names to be used in FileCheck regular expression to represent SSA "
        "variables in the order they are encountered. Separate names with commas, "
        "and leave empty entries for default names (e.g.: 'DIM,,SUM,RESULT')")
    parser.add_argument(
        "--attribute_names",
        type=str,
        default='',
        help="Names to be used in FileCheck regular expression to represent "
        "attributes in the order they are defined. Separate names with commas,"
        "commas, and leave empty entries for default names (e.g.: 'MAP0,,,MAP1')")

    args = parser.parse_args()

    # Open the given input file.
    input_lines = [l.rstrip() for l in args.input]
    args.input.close()

    # Generate a note used for the generated check file.
    script_name = os.path.basename(__file__)
    autogenerated_note = ADVERT_BEGIN + "utils/" + script_name + "\n" + ADVERT_END

    source_segments = None
    if args.source:
        source_segments = process_source_lines(
            [l.rstrip() for l in open(args.source, "r")], autogenerated_note, args
        )

    if args.inplace:
        assert args.output is None
        output = open(args.source, "w")
    elif args.output is None:
        output = sys.stdout
    else:
        output = args.output

    output_segments = [[]]

    # Namers
    variable_namer = VariableNamer(args.variable_names)
    attribute_namer = AttributeNamer(args.attribute_names)

    # Process lines
    for input_line in input_lines:
        if not input_line:
            continue

        # Check if this is an attribute definition and process it
        process_attribute_definition(input_line, attribute_namer, output)

        # Lines with blocks begin with a ^. These lines have a trailing comment
        # that needs to be stripped.
        lstripped_input_line = input_line.lstrip()
        is_block = lstripped_input_line[0] == "^"
        if is_block:
            input_line = input_line.rsplit("//", 1)[0].rstrip()

        cur_level = variable_namer.num_scopes()

        # If the line starts with a '}', pop the last name scope.
        if lstripped_input_line[0] == "}":
            variable_namer.pop_name_scope()
            cur_level = variable_namer.num_scopes()

        # If the line ends with a '{', push a new name scope.
        if input_line[-1] == "{":
            variable_namer.push_name_scope()
            if cur_level == args.starts_from_scope:
                output_segments.append([])

            # Result SSA values must still be pushed to parent scope
            num_ssa_results = get_num_ssa_results(input_line)
            variable_namer.generate_in_parent_scope(num_ssa_results)

        # Omit lines at the near top level e.g. "module {".
        if cur_level < args.starts_from_scope:
            continue

        if len(output_segments[-1]) == 0:
            variable_namer.clear_names()

        # Preprocess the input to remove any sequences that may be problematic with
        # FileCheck.
        input_line = preprocess_line(input_line)

        # Process uses of attributes in this line
        input_line = process_attribute_references(input_line, attribute_namer)

        # Split the line at the each SSA value name.
        ssa_split = input_line.split("%")

        # If this is a top-level operation use 'CHECK-LABEL', otherwise 'CHECK:'.
        if len(output_segments[-1]) != 0 or not ssa_split[0]:
            output_line = "// " + args.check_prefix + ": "
            # Pad to align with the 'LABEL' statements.
            output_line += " " * len("-LABEL")

            # Output the first line chunk that does not contain an SSA name.
            output_line += ssa_split[0]

            # Process the rest of the input line.
            output_line += process_line(ssa_split[1:], variable_namer)

        else:
            # Output the first line chunk that does not contain an SSA name for the
            # label.
            output_line = "// " + args.check_prefix + "-LABEL: " + ssa_split[0] + "\n"

            # Process the rest of the input line on separate check lines.
            for argument in ssa_split[1:]:
                output_line += "// " + args.check_prefix + "-SAME:  "

                # Pad to align with the original position in the line.
                output_line += " " * len(ssa_split[0])

                # Process the rest of the line.
                output_line += process_line([argument], variable_namer)

        # Append the output line.
        output_segments[-1].append(output_line)

    output.write(autogenerated_note + "\n")

    output_segments = list(filter(None, output_segments))
    # Write the output.
    if source_segments:
        assert len(output_segments) == len(source_segments), f"{len(output_segments)=}, {len(source_segments)=}"
        for check_segment, source_segment in zip(output_segments, source_segments):
            for line in check_segment:
                output.write(line)
            for line in source_segment:
                output.write(line)
    else:
        for segment in output_segments:
            output.write("\n")
            for output_line in segment:
                output.write(output_line)
        output.write("\n")
    output.close()


if __name__ == "__main__":
    main()
