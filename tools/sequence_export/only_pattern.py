# SPDX-License-Identifier: MIT

"""Builds the converters' --only patterns for the checks that convert a list of sequences, a batch at a time.

only_pattern(names) returns a pattern that matches each of `names` whole and nothing else. The converters read --only
with std::regex's ECMAScript grammar. escape(name) puts a backslash before each character that the grammar treats as
special outside a class, and before the slash. It leaves every other character as it is: in names such as "SE-01" or
"BGM 2", a hyphen or a space matches itself. batches(names, size) splits `names` into lists of at most `size`.
"""

# The characters that the ECMAScript grammar of regular expressions treats as special, and the slash.
SPECIAL = set("\\^$.|?*+()[]{}/")


def escape(name):
    return "".join("\\" + c if c in SPECIAL else c for c in name)


def only_pattern(names):
    return "^(?:" + "|".join(escape(name) for name in names) + ")$"


def batches(names, size):
    return [names[i : i + size] for i in range(0, len(names), size)]
