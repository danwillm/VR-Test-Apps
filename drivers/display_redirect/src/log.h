#pragma once

#include <openvr_driver.h>

#include <cstdarg>
#include <cstdio>

namespace sample
{
inline void Log( const char *pchFormat, ... )
{
    char buffer[ 2048 ]{};

    va_list args;
    va_start( args, pchFormat );
    vsnprintf_s( buffer, sizeof( buffer ), _TRUNCATE, pchFormat, args );
    va_end( args );

    if ( vr::VRDriverLog() )
    {
        vr::VRDriverLog()->Log( buffer );
    }
}
} // namespace sample
