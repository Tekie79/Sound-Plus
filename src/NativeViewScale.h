#pragma once
#include <optional>
namespace sp
{
// Per native view lifetime. SDK success (0) means the view accepted the request,
// including a successful pre-attachment request. Rejected/unhandled requests
// are never cached and are retried after attachment or on the next update.
class NativeViewScale
{
    std::optional<float> accepted;

  public:
    template <class Apply> int apply(float scale, Apply &&setScale)
    {
        if (accepted && *accepted == scale)
            return 0;
        const int result = setScale(scale);
        if (result == 0)
            accepted = scale;
        return result;
    }
};
} // namespace sp
