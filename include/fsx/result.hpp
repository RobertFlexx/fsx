#pragma once
#include "fsx/error.hpp"
#include <optional>
#include <utility>

namespace fsx {

template <class T>
    class Result {
    public:
        Result(T value) : value_(std::move(value)) {
        }
        Result(Error error) : error_(std::move(error)) {
        }

        explicit operator bool() const noexcept { return value_.has_value();
        }
        T& value() & { return *value_;
        }
        const T& value() const & { return *value_;
        }
        T && value() && { return std::move(*value_);
        }
        const Error& error() const noexcept { return error_;
        }

    private:
        std::optional<T> value_;
        Error error_{};
    };

template <>
    class Result<void> {
    public:
        Result() : ok_(true) {
        }
        Result(Error error) : ok_(false), error_(std::move(error)) {
        }
        explicit operator bool() const noexcept { return ok_;
        }
        const Error& error() const noexcept { return error_;
        }
    private:
        bool ok_{false};
        Error error_{};
    };

} // namespace fsx
