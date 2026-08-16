#include "core/Calculator.h"

#include <QVector>
#include <QChar>

#include <cmath>
#include <stdexcept>

namespace Calculator {
namespace {

// ---- token types ----
enum class TokenType { Number, Plus, Minus, Star, Slash, LParen, RParen, End };

struct Token
{
    TokenType type;
    double    value = 0.0;  // meaningful only for Number
};

// ---- lexer ----
class Lexer
{
public:
    explicit Lexer(const QString &source)
        : m_src(source), m_pos(0) {}

    Token peek() const { return m_lookahead; }

    Token advance()
    {
        Token t = m_lookahead;
        m_lookahead = nextToken();
        return t;
    }

    /// Must be called once before parsing starts.
    void init()
    {
        m_lookahead = nextToken();
    }

private:
    Token nextToken()
    {
        skipWhitespace();
        if (m_pos >= m_src.length())
            return { TokenType::End };

        QChar c = m_src[m_pos];

        switch (c.unicode()) {
        case '+': ++m_pos; return { TokenType::Plus };
        case '-': ++m_pos; return { TokenType::Minus };
        case '*': ++m_pos; return { TokenType::Star };
        case '/': ++m_pos; return { TokenType::Slash };
        case '(': ++m_pos; return { TokenType::LParen };
        case ')': ++m_pos; return { TokenType::RParen };
        default:
            if (c.isDigit() || c == QLatin1Char('.'))
                return readNumber();
            // Unknown character — will cause parse error
            ++m_pos;
            return { TokenType::End };
        }
    }

    void skipWhitespace()
    {
        while (m_pos < m_src.length() && m_src[m_pos].isSpace())
            ++m_pos;
    }

    Token readNumber()
    {
        int start = m_pos;
        bool hasDot = false;

        while (m_pos < m_src.length()) {
            QChar c = m_src[m_pos];
            if (c.isDigit()) {
                ++m_pos;
            } else if (c == QLatin1Char('.') && !hasDot) {
                hasDot = true;
                ++m_pos;
            } else {
                break;
            }
        }

        bool ok = false;
        double val = m_src.mid(start, m_pos - start).toDouble(&ok);
        return { TokenType::Number, ok ? val : 0.0 };
    }

    QString    m_src;
    int        m_pos;
    Token      m_lookahead;
};

// ---- recursive-descent parser ----

class Parser
{
public:
    explicit Parser(Lexer &lexer) : m_lex(lexer) {}

    double parse()
    {
        double val = expr();
        if (m_lex.peek().type != TokenType::End)
            throw std::runtime_error("Unexpected token after expression");
        return val;
    }

private:
    // expr → term (('+' | '-') term)*
    double expr()
    {
        double left = term();
        while (true) {
            TokenType t = m_lex.peek().type;
            if (t == TokenType::Plus) {
                m_lex.advance();
                left += term();
            } else if (t == TokenType::Minus) {
                m_lex.advance();
                left -= term();
            } else {
                break;
            }
        }
        return left;
    }

    // term → factor (('*' | '/') factor)*
    double term()
    {
        double left = factor();
        while (true) {
            TokenType t = m_lex.peek().type;
            if (t == TokenType::Star) {
                m_lex.advance();
                left *= factor();
            } else if (t == TokenType::Slash) {
                m_lex.advance();
                double rhs = factor();
                if (std::abs(rhs) < 1e-15)
                    throw std::runtime_error("Division by zero");
                left /= rhs;
            } else {
                break;
            }
        }
        return left;
    }

    // factor → ('+' | '-') factor | '(' expr ')' | NUMBER
    double factor()
    {
        TokenType t = m_lex.peek().type;

        // Unary plus
        if (t == TokenType::Plus) {
            m_lex.advance();
            return factor();
        }

        // Unary minus
        if (t == TokenType::Minus) {
            m_lex.advance();
            return -factor();
        }

        // Parenthesised expression
        if (t == TokenType::LParen) {
            m_lex.advance();
            double val = expr();
            if (m_lex.peek().type != TokenType::RParen)
                throw std::runtime_error("Missing ')'");
            m_lex.advance();
            return val;
        }

        // Number
        if (t == TokenType::Number) {
            return m_lex.advance().value;
        }

        throw std::runtime_error("Expected number or '('");
    }

    Lexer &m_lex;
};

} // anonymous namespace

// ---- public API ----

bool tryParse(const QString &expr, double *result, QString *error)
{
    if (!result)
        return false;

    try {
        Lexer lexer(expr);
        lexer.init();
        Parser parser(lexer);
        *result = parser.parse();
        return true;
    } catch (const std::runtime_error &e) {
        if (error)
            *error = QString::fromUtf8(e.what());
        return false;
    }
}

} // namespace Calculator
