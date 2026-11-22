/******************************************************************************
*                 SOFA, Simulation Open-Framework Architecture                *
*                    (c) 2006 INRIA, USTL, UJF, CNRS, MGH                     *
*                                                                             *
* This program is free software; you can redistribute it and/or modify it     *
* under the terms of the GNU Lesser General Public License as published by    *
* the Free Software Foundation; either version 2.1 of the License, or (at     *
* your option) any later version.                                             *
*                                                                             *
* This program is distributed in the hope that it will be useful, but WITHOUT *
* ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or       *
* FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License *
* for more details.                                                           *
*                                                                             *
* You should have received a copy of the GNU Lesser General Public License    *
* along with this program. If not, see <http://www.gnu.org/licenses/>.        *
*******************************************************************************
* Authors: The SOFA Team and external contributors (see Authors.txt)          *
*                                                                             *
* Contact information: contact@sofa-framework.org                             *
******************************************************************************/
#include <sofa/helper/system/DynamicLibrary.h>
#include <sofa/helper/system/FileSystem.h>
using sofa::helper::system::FileSystem;
#include <sofa/helper/logging/Messaging.h>
#ifdef WIN32
# include <Windows.h>
#else
# include <dlfcn.h>
#endif
#include <string>
#include <filesystem>
#include <sstream>

#if defined(__EMSCRIPTEN__)

#include <emscripten.h>

EM_ASYNC_JS(int, fetchAsset, (const char* load_url_ptr, const char* save_path_ptr), {
    const url = UTF8ToString(load_url_ptr);
    const path = UTF8ToString(save_path_ptr);
    try {
        const response = await fetch(url);
        if (!response.ok) {
            out("response no OK");
            return 0;
        }
        const buffer = await response.arrayBuffer();
        FS.writeFile(path, new Uint8Array(buffer));
        return 1;
    } catch (e) {
        console.error("Failed to fetch side module:", e);
        return 0;
    }
    return 1;
});

#endif

namespace sofa::helper::system
{


DynamicLibrary::Handle::Handle(const std::string& filename, void *handle)
    : m_realHandle(handle), m_filename(std::make_shared<std::string>(filename))
{
}

DynamicLibrary::Handle::Handle(const Handle& that)
    : m_realHandle(that.m_realHandle), m_filename(that.m_filename)
{
}

DynamicLibrary::Handle::Handle(): m_realHandle(nullptr)
{
}

DynamicLibrary::Handle& DynamicLibrary::Handle::operator= (const Handle& other) {
    m_realHandle = other.m_realHandle;
    m_filename = other.m_filename;
    return *this;
}

bool DynamicLibrary::Handle::isValid() const
{
    return m_realHandle != nullptr;
}

const std::string& DynamicLibrary::Handle::filename() const
{
    return *m_filename;
}

DynamicLibrary::Handle DynamicLibrary::load(const std::string& filename)
{
#if defined(__EMSCRIPTEN__)
    // We need to first download the side module for access from main module
    const std::string savePath = "/" + filename;
    if(!fetchAsset(filename.c_str(),savePath.c_str())){
         msg_error("DynamicLibrary") << "Error downloading plugin" << filename;
    }
#endif
# if defined(WIN32)
    const std::string p = FileSystem::cleanPath(filename, FileSystem::BACKSLASH);
    void *handle = ::LoadLibraryA(filename.c_str());
    if (handle == nullptr)
    {
        // Retry with altered search path
        handle = ::LoadLibraryExA(p.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    }
# else
    void *handle = ::dlopen(filename.c_str(), RTLD_NOW | RTLD_GLOBAL);
# endif
    if (handle == nullptr){
        msg_error("DynamicLibrary") << "Error loading library " << filename;
        fetchLastError();
    }
    return handle ? Handle(filename, handle) : Handle();
}

int DynamicLibrary::unload(Handle handle)
{
# if defined(WIN32)
    int error = (::FreeLibrary((HMODULE)(handle.m_realHandle)) == 0);
# else
    int error = ::dlclose(handle.m_realHandle);
# endif
    if (error)
        fetchLastError();
    return error;
}

void * DynamicLibrary::getSymbolAddress(Handle handle,
                                        const std::string& symbol)
{
    if (!handle.isValid()) {
        m_lastError = "DynamicLibrary::getSymbolAddress(): invalid handle";
        msg_error("DynamicLibrary") << "Error getting symbol " << m_lastError;
        return nullptr;
    }
# if defined(WIN32)
    void *symbolAddress = ::GetProcAddress((HMODULE)handle.m_realHandle,
                                     symbol.c_str());
# else
    void *symbolAddress = ::dlsym(handle.m_realHandle, symbol.c_str());
# endif
    if(symbolAddress == nullptr){
        msg_error("DynamicLibrary") << "Error: null symbol address for " << symbol;
        fetchLastError();
    }
#if !defined (WIN32) && !defined (__EMSCRIPTEN__)
    else // checking that the symbol really comes from the provided library
    {

        Dl_info dli;
        if (::dladdr(symbolAddress, &dli) ==0){
            msg_warning("DynamicLibrary") << "Symbol address could not be matched to a shared object.";
        }
        std::filesystem::path dlInfoPath;
        std::filesystem::path handlePath;
        std::ostringstream oss;

        try
        {
            dlInfoPath = std::filesystem::canonical(dli.dli_fname);
            handlePath = std::filesystem::canonical(handle.filename());
        }
        catch(std::filesystem::filesystem_error& error)
        {
            oss << "An exception was caught when trying to find the real path of the library with the following message : " << error.what();
            msg_error("DynamicLibrary") << "Error getting real path for library: " << error.what();

            // paths are broken, we cannot check if this symbol comes from this lib
            symbolAddress = nullptr;
            m_lastError = oss.str();

            return symbolAddress;
        }


        // Both paths should be exactly the same
        if(dlInfoPath.compare(handlePath) != 0)
        {
            oss << symbol << " was found in the library " << dlInfoPath
                << " , but it should have been found in this library " << handlePath << "\n."
                << "The most probable reason is that your requested library " << handlePath.filename()
                << " does not implement the symbol " << symbol << ", but its dependency "
                << dlInfoPath.filename()<< " does.";

            // the symbol was found in an other library (dependency)
            symbolAddress = nullptr;
            m_lastError = oss.str();
        }
    }
#endif
    return symbolAddress;
}

std::string DynamicLibrary::getLastError()
{
    std::string msg = m_lastError;
    m_lastError = "";
    return msg;
}

void DynamicLibrary::fetchLastError()
{
#if defined(WIN32)
    LPTSTR pMsgBuf;
    FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER
                  | FORMAT_MESSAGE_FROM_SYSTEM
                  | FORMAT_MESSAGE_IGNORE_INSERTS,
                  nullptr, ::GetLastError(),
                  MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                  (LPTSTR)&pMsgBuf, 0, nullptr);
# ifndef UNICODE
    m_lastError = std::string(pMsgBuf);
# else
    std::wstring s(pMsgBuf);
    // Convert wide string to narrow string properly
    int size = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0, NULL, FALSE);
    if (size > 0)
    {
        m_lastError.resize(size - 1);
        WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, &m_lastError[0], size, NULL, FALSE);
    }
# endif
    LocalFree(pMsgBuf);
#else
    const char *dlopenError = ::dlerror();
    m_lastError = dlopenError ? dlopenError : "";
#endif
    msg_error("DynamicLibrary") << "Last error: " << m_lastError;
}


#if defined(WIN32)
const std::string DynamicLibrary::extension = "dll";
#elif defined(__APPLE__)
const std::string DynamicLibrary::extension = "dylib";
#else
const std::string DynamicLibrary::extension = "so";
#endif

#if defined(WIN32)
const std::string DynamicLibrary::prefix = "";
#else
const std::string DynamicLibrary::prefix = "lib";
#endif


std::string DynamicLibrary::m_lastError = std::string("");


}
