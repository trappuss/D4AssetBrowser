#include "app/SehGuard.h"

#include <QString>

#if defined(_MSC_VER)
#  include <windows.h>
#  include <eh.h>       // _set_se_translator
#  include <exception>
#endif

namespace seh {

#if defined(_MSC_VER)

namespace {

// Thrown from the SE translator; carries the SEH code so the C++ catch can log
// it. Kept as a plain type (not derived from std::exception) so it can only be
// caught deliberately by runGuarded().
struct SehThrown {
    unsigned long code;
};

QString codeName(unsigned long code)
{
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:      return QStringLiteral("access violation");
        case EXCEPTION_ILLEGAL_INSTRUCTION:   return QStringLiteral("illegal instruction");
        case EXCEPTION_INT_DIVIDE_BY_ZERO:    return QStringLiteral("integer divide by zero");
        case EXCEPTION_STACK_OVERFLOW:        return QStringLiteral("stack overflow");
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:    return QStringLiteral("float divide by zero");
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return QStringLiteral("array bounds exceeded");
        case EXCEPTION_IN_PAGE_ERROR:         return QStringLiteral("in-page error");
        case EXCEPTION_DATATYPE_MISALIGNMENT: return QStringLiteral("datatype misalignment");
        default: break;
    }
    return QStringLiteral("SEH 0x%1").arg(code, 8, 16, QLatin1Char('0'));
}

void translator(unsigned int code, EXCEPTION_POINTERS*)
{
    throw SehThrown{ static_cast<unsigned long>(code) };
}

} // namespace

void installSehTranslator()
{
    // Per-thread; overwriting with the same function is harmless.
    _set_se_translator(translator);
}

bool runGuarded(const char* stage, const std::function<void()>& fn,
                HardwareFault* outFault)
{
    installSehTranslator();
    try {
        fn();
        return true;
    } catch (const SehThrown& e) {
        if (outFault) {
            outFault->code = e.code;
            outFault->what = QStringLiteral("%1 during %2")
                                 .arg(codeName(e.code),
                                      QString::fromLatin1(stage ? stage : "?"));
        }
        return false;
    } catch (const std::exception& e) {
        if (outFault) {
            outFault->code = 0;
            outFault->what = QStringLiteral("%1 during %2")
                                 .arg(QString::fromLocal8Bit(e.what()),
                                      QString::fromLatin1(stage ? stage : "?"));
        }
        return false;
    } catch (...) {
        if (outFault) {
            outFault->code = 0;
            outFault->what = QStringLiteral("unknown exception during %1")
                                 .arg(QString::fromLatin1(stage ? stage : "?"));
        }
        return false;
    }
}

#else // non-MSVC: best-effort C++-exception guard only (no SEH on this target).

void installSehTranslator() {}

bool runGuarded(const char* stage, const std::function<void()>& fn,
                HardwareFault* outFault)
{
    try {
        fn();
        return true;
    } catch (const std::exception& e) {
        if (outFault) {
            outFault->code = 0;
            outFault->what = QStringLiteral("%1 during %2")
                                 .arg(QString::fromLocal8Bit(e.what()),
                                      QString::fromLatin1(stage ? stage : "?"));
        }
        return false;
    } catch (...) {
        if (outFault) {
            outFault->code = 0;
            outFault->what = QStringLiteral("unknown exception during %1")
                                 .arg(QString::fromLatin1(stage ? stage : "?"));
        }
        return false;
    }
}

#endif

} // namespace seh
