#include "jbc/consumer/consumer.hpp"

namespace jbc::consumer {

const char* to_string(ConsumerError e) noexcept
{
    switch (e) {
        case ConsumerError::Ok: return "ok";
        case ConsumerError::NotFound: return "class not found in source";
        case ConsumerError::BadClassFile: return "class bytes failed to decode";
        case ConsumerError::RenameRule: return "rename plan rule rejected";
        case ConsumerError::RenameFailed: return "rename applier failed";
        case ConsumerError::WriteFailed: return "class write failed";
    }
    return "unknown";
}

}   // namespace jbc::consumer
