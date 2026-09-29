// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// AltaStor file backup implementation
//

#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <vector>
#include <system_error>
#include <filesystem>
namespace fs = std::filesystem;

#include "file_backup.h"
#include "abr_backup_format.h"
#include "backup_core.h"

namespace Abr
{
    //
    // File backup implementation
    //
    class FileBackup : public Abr::BackupCore
    {
    public:
        FileBackup(std::shared_ptr<Abr::IBackupManager> pManager)
            : Abr::BackupCore(pManager)
        {
        }

        ~FileBackup() override
        {
            // Removes the files of a backup that was never finalized, for example
            // because the caller gave up on it without calling finalizeBackup().
            discardPartialBackup();
        }

    protected:
        Handle createFile(const std::string &fullName, bool partialWrite) override;

        bool writeFile(Handle hFile, void *pDataBuf, uint32_t dataLength) override;

        bool closeFile(Handle hFile, bool bDelete = false) override;

        bool deleteFile(const std::string &fullName) override;
    };

    //
    // S3 restore from backup implementation
    //
    class FileRestore : public Abr::RestoreCore
    {
    public:
        FileRestore(std::shared_ptr<Abr::IBackupManager> pManager)
            : Abr::RestoreCore(pManager)
        {
        }

        ~FileRestore() override
        {
            if (m_hDataFile != nullptr) {
                close();
            }
        }
        
    protected:   
        Handle openFile(const std::string &fullName) override;

        bool readFile(Handle hFile, void *pDataBuf, uint64_t offset, uint32_t dataLength) override;

        bool closeFile(Handle hFile) override;

        bool deleteFile(const std::string &fullName) override;
    };
}

////////////

namespace
{
    // The modes below are POSIX ones. On Windows std::filesystem can only turn them
    // into the read-only attribute -- which they leave off, since the owner may
    // write -- and who can read a repository there is down to its ACL instead.

    // rwxr-x--- : full access for the owner, traverse/read for the group, nothing for others.
    constexpr fs::perms BACKUP_FOLDER_PERMS =
        fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec;

    // rw-r----- : the data files need no exec bit, and must never be readable by others
    constexpr fs::perms BACKUP_FILE_PERMS =
        fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read;

    std::string permsToOctal(fs::perms perms)
    {
        std::ostringstream os;
        os << std::oct << std::setfill('0') << std::setw(3)
           << (static_cast<unsigned>(perms) & static_cast<unsigned>(fs::perms::all));
        return os.str();
    }

    //
    // Removes a file from the repository. A file that is not there counts as
    // removed: deleting a partially written backup must not fail over the half of
    // it that was never created.
    //
    bool removeRepositoryFile(const fs::path &filePath)
    {
        std::error_code ec;

        if (fs::remove(filePath, ec) || !ec) {
            return true;
        }

        std::cerr << "Failed to delete '" << filePath.string() << "': " << ec.message() << std::endl;
        return false;
    }

    //
    // Creates dirPath together with every missing parent, each of them with BACKUP_FOLDER_PERMS.
    // create_directories() cannot be used directly because it applies the process umask
    // and reports only the deepest created directory.
    //
    bool createProtectedDirectories(const fs::path &dirPath, std::error_code &ec)
    {
        ec.clear();

        std::vector<fs::path> missing;
        std::error_code existsEc;
        for (fs::path path = dirPath; !path.empty() && !fs::exists(path, existsEc); path = path.parent_path()) {
            missing.push_back(path);
            if (path.parent_path() == path) {
                break;
            }
        }

        // Create the shallowest level first, so every new directory gets the restrictive mode
        for (auto it = missing.rbegin(); it != missing.rend(); ++it) {
            if (!fs::create_directory(*it, ec) && ec) {
                return false;
            }

            fs::permissions(*it, BACKUP_FOLDER_PERMS, fs::perm_options::replace, ec);
            if (ec) {
                return false;
            }
        }

        return true;
    }
}

Abr::FileBackupManager::~FileBackupManager()
{
}

bool Abr::FileBackupManager::initialize(const Abr::BackupManagerParams& params)
{
    if(params.m_folder.empty()) {
        std::cerr << "Folder name is empty" << std::endl;
        return false;
    }

    fs::path dirPath = params.m_folder;
    std::error_code ec;

    if (!fs::exists(dirPath, ec)) {
        if (!params.m_createFolderIfMissing) {
            std::cerr << "Directory does not exist '" << params.m_folder << "'!" << std::endl;
            return false;
        }

        if (!createProtectedDirectories(dirPath, ec)) {
            std::cerr << "Failed to create directory '" << params.m_folder << "' with "
                      << permsToOctal(BACKUP_FOLDER_PERMS) << " permissions: " << ec.message() << std::endl;
            return false;
        }
    } else if (!fs::is_directory(dirPath, ec)) {
        std::cerr << "'" << params.m_folder << "' is not a directory!" << std::endl;
        return false;
    } else {
#ifdef _WIN32
        // Nothing to check: Windows has no mode bits, and std::filesystem makes
        // them up from the read-only attribute, so every writable folder reads
        // as 0777 and none could pass the check below. The folder's ACL, which
        // it inherits from where it was created, decides who can read it.
#else
        // An existing folder is only used when it is not more permissive than 0750,
        // otherwise the backup content would be readable/writable by unrelated users
        fs::perms perms = fs::status(dirPath, ec).permissions();
        if (ec) {
            std::cerr << "Failed to read permissions of '" << params.m_folder << "': " << ec.message() << std::endl;
            return false;
        }

        fs::perms extra = perms & fs::perms::all & ~BACKUP_FOLDER_PERMS;
        if (extra != fs::perms::none) {
            if (!params.m_createFolderIfMissing) {
                std::cerr << "Directory '" << params.m_folder << "' has permissions " << permsToOctal(perms)
                          << ", expected " << permsToOctal(BACKUP_FOLDER_PERMS) << " or stricter!" << std::endl;
                return false;
            }

            // The caller asked us to set the folder up, so drop the extra bits instead of
            // refusing to use it. Only the excess is removed, a stricter mode is kept as is.
            fs::permissions(dirPath, extra, fs::perm_options::remove, ec);
            if (ec) {
                std::cerr << "Failed to change permissions of '" << params.m_folder << "' from "
                          << permsToOctal(perms) << " to " << permsToOctal(perms & ~extra) << ": "
                          << ec.message() << std::endl;
                return false;
            }

            std::cerr << "Changed permissions of '" << params.m_folder << "' from " << permsToOctal(perms)
                      << " to " << permsToOctal(perms & ~extra) << "." << std::endl;
        }
#endif
    }

    m_folder = params.m_folder;
    
    return true;
}

bool Abr::FileBackupManager::listBackups(const std::string &path, std::vector<std::string> &backups)
{
    std::string fullPath = m_folder + "/" + path;
    fs::path dir_path = fullPath;

    try {
        if (fs::exists(dir_path) && fs::is_directory(dir_path)) {
            for (const auto& entry : fs::directory_iterator(dir_path)) {
                if (fs::is_regular_file(entry.status())) {
                    // .string() rather than a conversion: a path converts to its native
                    // string, which is std::wstring on Windows.
                    std::string ext = entry.path().extension().string();
                    if(ext == Abr::INDEX_FILE_EXT) {
                        std::string name = entry.path().filename().string();
                        name.erase(name.length() - ext.length());
                        if (!name.empty()) {
                            backups.push_back(name);
                        }
                    }
                }
            }
        }
    } catch (const fs::filesystem_error& e) {
        std::cerr << "Error: " << e.what() << "\n";
    }

    return true;
}

bool Abr::FileBackupManager::createBackup(const std::string &path, const std::string &name,
    const BackupInfo& info, std::shared_ptr<IBackup> &pBackup, const std::string encryptionKey)
{
    std::shared_ptr<FileBackupManager> self = shared_from_this();
    auto backup = std::make_shared<FileBackup>(self);

    if (!backup->create(path, name, info, encryptionKey)) {
        std::cerr << "Failed to create backup " << name << " in " << path << std::endl;
        return false;
    }

    pBackup = backup;

    return true;
}

bool Abr::FileBackupManager::openBackup(const std::string &path, const std::string &name,
     std::shared_ptr<IRestore> &pRestore, const std::string decryptionKey)
{
    std::shared_ptr<FileBackupManager> self = shared_from_this();
    auto backup = std::make_shared<FileRestore>(self);

    if (!backup->open(path, name, decryptionKey)) {
        std::cerr << "Failed to open backup " << name << " in " << path << std::endl;
        return false;
    }

    pRestore = backup;

    return true;
}

bool Abr::FileBackupManager::loadMetaData(const std::string &path, const std::string &name, std::ostream &metaData)
{
    std::string filePath = getFolder() + "/" + (!path.empty() ? path + "/" : "") + name + Abr::META_FILE_EXT;

    auto file = std::make_shared<std::ifstream>(filePath, std::ios::in | std::ios::binary);

    if (!file || file->fail()) {
        std::cerr << "Failed to open file '" << filePath << "'!" << std::endl;
        return false;
    }

    metaData << file->rdbuf();

    // Inserting a streambuf raises failbit when it transfers zero characters,
    // which is exactly what empty metadata does; only badbit signals a real
    // read/write error.
    if (metaData.bad() || file->bad()) {
        std::cerr << "Failed to write metadata from '" << filePath << "' to output stream!" << std::endl;
        return false;
    }

    return true;
}

bool Abr::FileBackupManager::saveMetaData(const std::string &path, const std::string &name, std::istream &metaData)
{
    std::string filePath = getFolder() + "/" + (!path.empty() ? path + "/" : "") + name + Abr::META_FILE_EXT;

    std::filesystem::path parentPath = std::filesystem::path(filePath).parent_path();
    if (!parentPath.empty() && !std::filesystem::exists(parentPath)) {
        std::error_code ec;
        if (!std::filesystem::create_directories(parentPath, ec) && ec) {
            std::cerr << "Failed to create directories for '" << parentPath.string() << "': " << ec.message() << std::endl;
            return false;
        }
    }

    auto file = std::make_shared<std::ofstream>(filePath, std::ios::out | std::ios::binary);

    if (!file || file->fail()) {
        std::cerr << "Failed to open file '" << filePath << "'!" << std::endl;
        return false;
    }

    (*file) << metaData.rdbuf();
    file->flush();

    // Inserting a streambuf raises failbit when it transfers zero characters,
    // which is exactly what empty metadata does; only badbit signals a real
    // write error. Treating the empty case as a failure used to report an error
    // after the open had already truncated any previously stored metadata.
    if (file->bad()) {
        std::cerr << "Failed to write metadata to '" << filePath << "'!" << std::endl;
        return false;
    }

    return true;
}

bool Abr::FileBackupManager::deleteBackup(const std::string &path, const std::string &name)
{
    // The files are removed by name instead of through FileRestore::open(). What
    // a run that died before finalizeBackup() leaves behind has no readable index,
    // and opening it first made exactly those leftovers impossible to delete.
    const std::string basePath = getFolder() + "/" + join_backup_path(path, name);

    bool found = false;
    bool ok = true;

    for (const char *ext : { Abr::DATA_FILE_EXT, Abr::INDEX_FILE_EXT, Abr::META_FILE_EXT }) {
        fs::path filePath = basePath + ext;

        std::error_code ec;
        if (!fs::exists(filePath, ec)) {
            continue;
        }

        found = true;
        ok = removeRepositoryFile(filePath) && ok;
    }

    if (!found) {
        std::cerr << "Backup " << name << " not found in " << path << std::endl;
        return false;
    }

    if (!ok) {
        std::cerr << "Failed to remove backup " << name << " in " << path << std::endl;
    }

    return ok;
}

//////

class WriteFileHandle : public std::ofstream
{
public:
    WriteFileHandle(const std::string &filename)
        : std::ofstream(filename, std::ios::out | std::ios::binary )
        , m_filename(filename)
    {}

    std::string m_filename;
};

Handle Abr::FileBackup::createFile(const std::string &fullName, bool partialWrite)
{
    std::string path = m_pManager->getFolder() + "/" + fullName;

    if (!partialWrite && std::filesystem::exists(path)) {
        std::cerr << "Error: File '" << path << "' already exists!\n";
        return nullptr;
    }

    std::error_code ec;

    std::filesystem::path parentPath = std::filesystem::path(path).parent_path();
    if (!parentPath.empty() && !std::filesystem::exists(parentPath)) {
        if (!createProtectedDirectories(parentPath, ec)) {
            std::cerr << "Failed to create directories for '" << parentPath.string() << "': " << ec.message() << std::endl;
            return nullptr;
        }
    }

    WriteFileHandle *pFile = new WriteFileHandle(path);

    if (!pFile || pFile->fail()) {
        std::cerr << "Failed to create file '" << path << "'!\n";
        if (pFile) {
            delete pFile;
        }
        return nullptr;
    }

    // ofstream creates the file with 0666 & ~umask, tighten it to the backup mode
    std::filesystem::permissions(path, BACKUP_FILE_PERMS, std::filesystem::perm_options::replace, ec);
    if (ec) {
        std::cerr << "Failed to set " << permsToOctal(BACKUP_FILE_PERMS) << " permissions on '"
                  << path << "': " << ec.message() << std::endl;
        pFile->close();
        delete pFile;
        std::error_code removeEc;
        std::filesystem::remove(path, removeEc);
        return nullptr;
    }

    return pFile;
}

bool Abr::FileBackup::writeFile(Handle hFile, void *pDataBuf, uint32_t dataLength)
{
    WriteFileHandle *pFile = static_cast<WriteFileHandle*>(hFile);

    if (pFile->fail()) {
        return false;
    }

    std::streampos posBefore = pFile->tellp();

    pFile->write((char*)pDataBuf, dataLength);

    uint32_t written = pFile->tellp() - posBefore;

    if (written != dataLength) {
        std::cerr << "Write error, expected " << dataLength << " bytes, written " << written << " bytes!" << std::endl;
        return false;
    }

    return true;
}

bool Abr::FileBackup::closeFile(Handle hFile, bool bDelete)
{
    WriteFileHandle *pFile = static_cast<WriteFileHandle*>(hFile);

    if (!pFile) {
        return true;
    }

    pFile->close();

    std::filesystem::path filePath = pFile->m_filename;

    delete pFile;

    // Report a failed delete: the caller is discarding a partial backup and has to
    // know that something of it is still in the repository.
    return bDelete ? removeRepositoryFile(filePath) : true;
}

bool Abr::FileBackup::deleteFile(const std::string &fullName)
{
    return removeRepositoryFile(m_pManager->getFolder() + "/" + fullName);
}

Handle Abr::FileRestore::openFile(const std::string &fullName)
{
    std::string path = m_pManager->getFolder() + "/" + fullName;

    std::ifstream *pFile = new std::ifstream(path, std::ios::in | std::ios::binary);

    if (!pFile || pFile->fail()) {
        std::cerr << "Failed to open file '" << path << "'!\n";
        delete pFile;
        return nullptr;
    }

    return pFile;
}

bool Abr::FileRestore::readFile(Handle hFile, void *pDataBuf, uint64_t offset, uint32_t dataLength)
{
    std::ifstream *pFile = static_cast<std::ifstream*>(hFile);

    if (pFile->fail()) {
        return false;
    }
    
    if (offset + dataLength > static_cast<uint64_t>(pFile->seekg(0, std::ifstream::end).tellg())) {
        std::cerr << "Error: Attempting to read beyond the end of the file!" << std::endl;
        return false;
    }

    if (pFile->tellg() != static_cast<std::streampos>(offset)) {
        pFile->seekg(offset);
    }

    pFile->read((char*)pDataBuf, dataLength);

    if (pFile->gcount() != dataLength) {
        std::cerr << "Error reading file, expected " << dataLength << " bytes, got " << pFile->gcount() << " bytes!" << std::endl;
        return false;
    }

    return true;
}

bool Abr::FileRestore::closeFile(Handle hFile)
{
    std::ifstream *pFile = static_cast<std::ifstream*>(hFile);

    pFile->close();
    delete pFile;

    return true;
}

bool Abr::FileRestore::deleteFile(const std::string &fullName)
{
    std::string path = m_pManager->getFolder() + "/" + fullName;

    std::filesystem::path filePath = path;
    bool ret = false;
    
    try {
        if (std::filesystem::remove(filePath)) {
           ret = true;
        }
    } catch (const std::filesystem::filesystem_error& e) {
        std::cerr << "Error: " << e.what() << "\n";
    }

    return ret;
}

