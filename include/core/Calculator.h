#pragma once

#include <QString>

/// Simple recursive-descent arithmetic evaluator.
///
/// Supports: + - * /  ( )  unary +/-
/// Follows standard PEMDAS precedence.
///
/// Usage:
///   double result;
///   QString error;
///   if (Calculator::tryParse("2 + 3 * 4", &result, &error)) {
///       // result == 14.0
///   }
namespace Calculator
{
    /// Attempt to parse and evaluate `expr`.
    /// Returns true on success, writing the numeric result to `result`.
    /// Returns false on parse error / division by zero, with a message in `error`.
    bool tryParse(const QString &expr, double *result, QString *error);
}
