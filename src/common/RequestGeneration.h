#pragma once

#include <cstdint>

namespace Spool {

class RequestGeneration final {
public:
    using Token = std::uint64_t;

    Token next()
    {
        return ++m_value;
    }
    void invalidate()
    {
        ++m_value;
    }
    Token current() const
    {
        return m_value;
    }
    bool isCurrent(Token token) const
    {
        return token == m_value;
    }

private:
    Token m_value = 0;
};

} // namespace Spool
