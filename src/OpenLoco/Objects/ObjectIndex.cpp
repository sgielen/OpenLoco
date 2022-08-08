#include "ObjectIndex.h"
#include "../Environment.h"
#include "../Interop/Interop.hpp"
#include "../Localisation/StringIds.h"
#include "../Localisation/StringManager.h"
#include "../OpenLoco.h"
#include "../Ui.h"
#include "../Ui/ProgressBar.h"
#include "../Utility/Numeric.hpp"
#include "../Utility/Stream.hpp"
#include "ObjectManager.h"
#include <cstdint>
#include <fstream>
#include <iostream>

using namespace OpenLoco::Interop;

namespace OpenLoco::ObjectManager
{
    static loco_global<std::byte*, 0x0050D13C> _installedObjectList;
    static loco_global<uint32_t, 0x0112A110> _installedObjectCount;
    static loco_global<bool, 0x0112A17E> _customObjectsInIndex;
    static loco_global<std::byte*, 0x0050D158> _50D158;
    static loco_global<std::byte[0x2002], 0x0112A17F> _112A17F;
    static loco_global<bool, 0x0050AEAD> _isFirstTime;
    static loco_global<bool, 0x0050D161> _isPartialLoaded;
    static loco_global<uint32_t, 0x009D9D52> _decodedSize;    // return of getScenarioText (badly named)
    static loco_global<uint32_t, 0x0112A168> _numImages;      // return of getScenarioText (badly named)
    static loco_global<uint8_t, 0x0112C211> _intelligence;    // return of getScenarioText (badly named)
    static loco_global<uint8_t, 0x0112C212> _aggressiveness;  // return of getScenarioText (badly named)
    static loco_global<uint8_t, 0x0112C213> _competitiveness; // return of getScenarioText (badly named)

#pragma pack(push, 1)
    struct ObjectFolderState
    {
        uint32_t numObjects = 0;
        uint32_t totalFileSize = 0;
        uint32_t dateHash = 0;
        constexpr bool operator==(const ObjectFolderState& rhs) const
        {
            return (numObjects == rhs.numObjects) && (totalFileSize == rhs.totalFileSize) && (dateHash == rhs.dateHash);
        }
        constexpr bool operator!=(const ObjectFolderState& rhs) const
        {
            return !(*this == rhs);
        }
    };
    static_assert(sizeof(ObjectFolderState) == 0xC);
    struct IndexHeader
    {
        ObjectFolderState state;
        uint32_t fileSize;
        uint32_t numObjects; // duplicates ObjectFolderState.numObjects but without high 1 and includes corrupted .dat's
    };
    static_assert(sizeof(IndexHeader) == 0x14);
#pragma pack(pop)

    // 0x00470F3C
    static ObjectFolderState getCurrentObjectFolderState()
    {
        ObjectFolderState currentState;
        const auto objectPath = Environment::getPathNoWarning(Environment::PathId::objects);
        for (const auto& file : fs::directory_iterator(objectPath, fs::directory_options::skip_permission_denied))
        {
            if (!file.is_regular_file())
            {
                continue;
            }
            const auto extension = file.path().extension().u8string();
            if (!Utility::iequals(extension, ".DAT"))
            {
                continue;
            }
            currentState.numObjects++;
            const auto lastWrite = file.last_write_time().time_since_epoch().count();
            currentState.dateHash ^= ((lastWrite >> 32) ^ (lastWrite & 0xFFFFFFFF));
            currentState.dateHash = Utility::ror(currentState.dateHash, 5);
            currentState.totalFileSize += file.file_size();
        }
        currentState.numObjects |= (1 << 24);
        return currentState;
    }

    // 0x00471712
    static bool hasCustomObjectsInIndex()
    {
        auto* ptr = *_installedObjectList;
        for (uint32_t i = 0; i < _installedObjectCount; i++)
        {
            auto entry = ObjectIndexEntry::read(&ptr);
            if (entry._header->isCustom())
                return true;
        }
        return false;
    }

    static void saveIndex(const IndexHeader& header)
    {
        std::ofstream stream;
        const auto indexPath = Environment::getPathNoWarning(Environment::PathId::plugin1);
        stream.open(indexPath, std::ios::out | std::ios::binary);
        if (!stream.is_open())
        {
            return;
        }
        stream.write(reinterpret_cast<const char*>(&header), sizeof(header));
        stream.write(reinterpret_cast<const char*>(*_installedObjectList), header.fileSize);
    }

    static std::pair<ObjectIndexEntry, size_t> createPartialNewEntry(std::byte* entryBuffer, const ObjectHeader& objHeader, const fs::path filename)
    {
        ObjectIndexEntry entry{};
        size_t newEntrySize = 0;

        // Header
        std::memcpy(&entryBuffer[newEntrySize], &objHeader, sizeof(objHeader));
        entry._header = reinterpret_cast<ObjectHeader*>(&entryBuffer[newEntrySize]);
        newEntrySize += sizeof(objHeader);

        // Filename
        std::strcpy(reinterpret_cast<char*>(&entryBuffer[newEntrySize]), filename.u8string().c_str());
        entry._filename = reinterpret_cast<char*>(&entryBuffer[newEntrySize]);
        newEntrySize += strlen(reinterpret_cast<char*>(&entryBuffer[newEntrySize])) + 1;

        // Header2 NULL
        ObjectHeader2 objHeader2;
        objHeader2.var_00 = std::numeric_limits<uint32_t>::max();
        std::memcpy(&entryBuffer[newEntrySize], &objHeader2, sizeof(objHeader2));
        newEntrySize += sizeof(objHeader2);

        // Name NULL
        entryBuffer[newEntrySize] = std::byte(0);
        entry._name = reinterpret_cast<char*>(&entryBuffer[newEntrySize]);
        newEntrySize += strlen(reinterpret_cast<char*>(&entryBuffer[newEntrySize])) + 1;

        // Header3 NULL
        ObjectHeader3 objHeader3{};
        std::memcpy(&entryBuffer[newEntrySize], &objHeader3, sizeof(objHeader3));
        newEntrySize += sizeof(objHeader3);

        // ObjectList1
        const uint8_t size1 = 0;
        entryBuffer[newEntrySize++] = std::byte(size1);

        // ObjectList2
        const uint8_t size2 = 0;
        entryBuffer[newEntrySize++] = std::byte(size2);

        return std::make_pair(entry, newEntrySize);
    }

    static std::pair<ObjectIndexEntry, size_t> createNewEntry(std::byte* entryBuffer, const ObjectHeader& objHeader, const fs::path filename)
    {
        ObjectIndexEntry entry{};
        size_t newEntrySize = 0;

        // Header
        std::memcpy(&entryBuffer[newEntrySize], &objHeader, sizeof(objHeader));
        entry._header = reinterpret_cast<ObjectHeader*>(&entryBuffer[newEntrySize]);
        newEntrySize += sizeof(objHeader);

        // Filename
        std::strcpy(reinterpret_cast<char*>(&entryBuffer[newEntrySize]), filename.u8string().c_str());
        entry._filename = reinterpret_cast<char*>(&entryBuffer[newEntrySize]);
        newEntrySize += strlen(reinterpret_cast<char*>(&entryBuffer[newEntrySize])) + 1;

        // Header2
        ObjectHeader2 objHeader2;
        objHeader2.var_00 = _decodedSize;
        std::memcpy(&entryBuffer[newEntrySize], &objHeader2, sizeof(objHeader2));
        newEntrySize += sizeof(objHeader2);

        // Name
        strcpy(reinterpret_cast<char*>(&entryBuffer[newEntrySize]), StringManager::getString(0x2000));
        entry._name = reinterpret_cast<char*>(&entryBuffer[newEntrySize]);
        newEntrySize += strlen(reinterpret_cast<char*>(&entryBuffer[newEntrySize])) + 1;

        // Header3
        ObjectHeader3 objHeader3{};
        objHeader3.var_00 = _numImages;
        objHeader3.intelligence = _intelligence;
        objHeader3.aggressiveness = _aggressiveness;
        objHeader3.competitiveness = _competitiveness;
        std::memcpy(&entryBuffer[newEntrySize], &objHeader3, sizeof(objHeader3));
        newEntrySize += sizeof(objHeader3);

        auto* ptr = &_112A17F[0];

        // ObjectList1 (required objects)
        const uint8_t size1 = uint8_t(*ptr++);
        entryBuffer[newEntrySize++] = std::byte(size1);
        std::memcpy(&entryBuffer[newEntrySize], ptr, sizeof(ObjectHeader) * size1);
        newEntrySize += sizeof(ObjectHeader) * size1;
        ptr += sizeof(ObjectHeader) * size1;

        // ObjectList2 (also loads objects {used for category selection})
        const uint8_t size2 = uint8_t(*ptr++);
        entryBuffer[newEntrySize++] = std::byte(size2);
        std::memcpy(&entryBuffer[newEntrySize], ptr, sizeof(ObjectHeader) * size2);
        newEntrySize += sizeof(ObjectHeader) * size2;
        ptr += sizeof(ObjectHeader) * size2;

        return std::make_pair(entry, newEntrySize);
    }

    // Adds a new object to the index by: 1. creating a partial index, 2. validating, 3. creating a full index entry
    static void addObjectToIndex(const fs::path filepath, size_t& usedBufferSize)
    {
        std::cout << "addObjectToIndex a" << std::endl;
        ObjectHeader objHeader{};
        {
            std::cout << "addObjectToIndex b" << std::endl;
            std::ifstream stream;
            stream.open(filepath, std::ios::in | std::ios::binary);
            std::cout << "addObjectToIndex c" << std::endl;
            Utility::readData(stream, objHeader);
            std::cout << "addObjectToIndex d" << std::endl;
            if (stream.gcount() != sizeof(objHeader))
            {
                return;
            }
            std::cout << "addObjectToIndex e" << std::endl;
        }

        std::cout << "addObjectToIndex f" << std::endl;
        const auto curObjPos = usedBufferSize;
        const auto partialNewEntry = createPartialNewEntry(&_installedObjectList[usedBufferSize], objHeader, filepath.filename());
        usedBufferSize += partialNewEntry.second;
        _installedObjectCount++;
        std::cout << "addObjectToIndex g" << std::endl;

        _isPartialLoaded = true;
        _50D158 = _112A17F;
        std::cout << "addObjectToIndex h" << std::endl;
        getScenarioText(objHeader);
        _50D158 = reinterpret_cast<std::byte*>(-1);
        _isPartialLoaded = false;
        std::cout << "addObjectToIndex i" << std::endl;
        if (_installedObjectCount == 0)
        {
            return;
        }
        _installedObjectCount--;
        std::cout << "addObjectToIndex j" << std::endl;

        // Rewind as it is only a partial object loaded
        usedBufferSize = curObjPos;

        // Load full entry into temp buffer.
        // 0x009D1CC8
        std::byte newEntryBuffer[0x2000] = {};
        std::cout << "addObjectToIndex k" << std::endl;
        const auto [newEntry, newEntrySize] = createNewEntry(newEntryBuffer, objHeader, filepath.filename());

        std::cout << "addObjectToIndex l" << std::endl;
        freeScenarioText();

        auto* indexPtr = *_installedObjectList;
        // This ptr will be pointing at where in the object list to install the new entry
        auto* installPtr = indexPtr;
        std::cout << "addObjectToIndex m" << std::endl;
        for (uint32_t i = 0; i < _installedObjectCount; i++)
        {
            std::cout << "addObjectToIndex n" << std::endl;
            auto entry = ObjectIndexEntry::read(&indexPtr);
            if (strcmp(newEntry._name, entry._name) < 0)
            {
                break;
            }
            // If cmp < 0 then we will want to install at the previous indexPtr location
            installPtr = indexPtr;
        }

        std::cout << "addObjectToIndex o" << std::endl;
        auto moveSize = usedBufferSize - (installPtr - *_installedObjectList);
        std::memmove(installPtr + newEntrySize, installPtr, moveSize);
        std::cout << "addObjectToIndex p" << std::endl;
        std::memcpy(installPtr, newEntryBuffer, newEntrySize);
        usedBufferSize += newEntrySize;

        std::cout << "addObjectToIndex q" << std::endl;
        _installedObjectCount++;
        std::cout << "addObjectToIndex r" << std::endl;
    }

    // 0x0047118B
    static void createIndex(const ObjectFolderState& currentState)
    {
        Ui::processMessagesMini();
        std::cout << "createIndex()" << std::endl;
        const auto progressString = _isFirstTime ? StringIds::starting_for_the_first_time : StringIds::checking_object_files;
        Ui::ProgressBar::begin(progressString);

        // Reset
        std::cout << "A" << std::endl;
        reloadAll();
        std::cout << "B" << std::endl;
        if (reinterpret_cast<int32_t>(*_installedObjectList) != -1)
        {
            std::cout << "C" << std::endl;
            free(*_installedObjectList);
        }

        std::cout << "D" << std::endl;
        // Prepare initial object list buffer (we will grow this as required)
        size_t bufferSize = 0x4000;
        _installedObjectList = static_cast<std::byte*>(malloc(bufferSize));
        if (_installedObjectList == nullptr)
        {
            std::cout << "E" << std::endl;
            _installedObjectList = reinterpret_cast<std::byte*>(-1);
            exitWithError(StringIds::unable_to_allocate_enough_memory, StringIds::game_init_failure);
            std::cout << "F" << std::endl;
            return;
        }

        std::cout << "G" << std::endl;
        _installedObjectCount = 0;
        // Create new index by iterating all DAT files and processing
        IndexHeader header{};
        uint8_t progress = 0;      // Progress is used for the ProgressBar Ui element
        size_t usedBufferSize = 0; // Keep track of used space to allow for growth and for final sizing
        std::cout << "H" << std::endl;
        const auto objectPath = Environment::getPathNoWarning(Environment::PathId::objects);
        std::cout << "I" << std::endl;
        for (const auto& file : fs::directory_iterator(objectPath, fs::directory_options::skip_permission_denied))
        {
            std::cout << "J " << file.path() << std::endl;
            if (!file.is_regular_file())
            {
                std::cout << "not a regular file" << std::endl;
                continue;
            }
            const auto extension = file.path().extension().u8string();
            std::cout << "extension: " << extension << std::endl;
            if (!Utility::iequals(extension, ".DAT"))
            {
                std::cout << "not extension DAT, ignoring" << std::endl;
                continue;
            }
            Ui::processMessagesMini();
            header.state.numObjects++;

            // Cheap calculation of (curObjectCount / totalObjectCount) * 256
            const auto newProgress = (header.state.numObjects << 8) / ((currentState.numObjects & 0xFFFFFF) + 1);
            std::cout << "newProgress " << newProgress << std::endl;
            if (progress != newProgress)
            {
                progress = newProgress;
                Ui::ProgressBar::setProgress(newProgress);
            }

            // Grow object list buffer if near limit
            const auto remainingBuffer = bufferSize - usedBufferSize;
            std::cout << "buffer calc: " << bufferSize << " - " << usedBufferSize << " = " << remainingBuffer << std::endl;
            if (remainingBuffer < 0x231E)
            {
                // Original grew buffer at slower rate. Memory is cheap though
                bufferSize *= 2;
                std::cout << "reallocating " << bufferSize << " bytes from " << (void*)*_installedObjectList << std::endl;
                _installedObjectList = static_cast<std::byte*>(realloc(*_installedObjectList, bufferSize));
                std::cout << "got: " << (void*)*_installedObjectList << std::endl;
                if (_installedObjectList == nullptr)
                {
                    std::cout << "unable to allocate enough memory" << std::endl;
                    exitWithError(StringIds::unable_to_allocate_enough_memory, StringIds::game_init_failure);
                    return;
                }
            }

            std::cout << "addObjectToIndex" << std::endl;
            addObjectToIndex(file.path(), usedBufferSize);
            std::cout << "K" << std::endl;
        }

        std::cout << "L" << std::endl;
        // New index creation completed. Reset and save result.
        reloadAll();
        header.fileSize = usedBufferSize;
        header.numObjects = _installedObjectCount;
        header.state = currentState;
        std::cout << "M" << std::endl;
        saveIndex(header);

        std::cout << "N" << std::endl;
        Ui::ProgressBar::end();
        std::cout << "O" << std::endl;
    }

    static bool tryLoadIndex(const ObjectFolderState& currentState)
    {
        const auto indexPath = Environment::getPathNoWarning(Environment::PathId::plugin1);
        if (!fs::exists(indexPath))
        {
            return false;
        }
        std::ifstream stream;
        stream.open(indexPath, std::ios::in | std::ios::binary);
        if (!stream.is_open())
        {
            return false;
        }
        // 0x00112A14C -> 160
        IndexHeader header{};
        Utility::readData(stream, header);
        if ((header.state != currentState) || (stream.gcount() != sizeof(header)))
        {
            return false;
        }
        else
        {
            if (reinterpret_cast<int32_t>(*_installedObjectList) != -1)
            {
                free(*_installedObjectList);
            }
            _installedObjectList = static_cast<std::byte*>(malloc(header.fileSize));
            if (_installedObjectList == nullptr)
            {
                exitWithError(StringIds::unable_to_allocate_enough_memory, StringIds::game_init_failure);
                return false;
            }
            Utility::readData(stream, *_installedObjectList, header.fileSize);
            if (stream.gcount() != static_cast<int32_t>(header.fileSize))
            {
                return false;
            }
            _installedObjectCount = header.numObjects;
            reloadAll();
        }
        return true;
    }

    // 0x00470F3C
    void loadIndex()
    {
        // 0x00112A138 -> 144
        const auto currentState = getCurrentObjectFolderState();

        if (!tryLoadIndex(currentState))
        {
            createIndex(currentState);
        }

        _customObjectsInIndex = hasCustomObjectsInIndex();
    }

    uint32_t getNumInstalledObjects()
    {
        return *_installedObjectCount;
    }

    std::vector<std::pair<uint32_t, ObjectIndexEntry>> getAvailableObjects(ObjectType type)
    {
        auto ptr = (std::byte*)_installedObjectList;
        std::vector<std::pair<uint32_t, ObjectIndexEntry>> list;

        for (uint32_t i = 0; i < _installedObjectCount; i++)
        {
            auto entry = ObjectIndexEntry::read(&ptr);
            if (entry._header->getType() == type)
                list.emplace_back(std::pair<uint32_t, ObjectIndexEntry>(i, entry));
        }

        return list;
    }

    bool isObjectInstalled(const ObjectHeader& objectHeader)
    {
        const auto objects = getAvailableObjects(objectHeader.getType());
        auto res = std::find_if(std::begin(objects), std::end(objects), [&objectHeader](auto& obj) { return *obj.second._header == objectHeader; });
        return res != std::end(objects);
    }

    // 0x00472AFE
    ObjIndexPair getActiveObject(ObjectType objectType, uint8_t* edi)
    {
        const auto objects = getAvailableObjects(objectType);

        for (auto [index, object] : objects)
        {
            if (edi[index] & (1 << 0))
            {
                return { static_cast<int16_t>(index), object };
            }
        }

        return { -1, ObjectIndexEntry{} };
    }

    ObjectIndexEntry ObjectIndexEntry::read(std::byte** ptr)
    {
        ObjectIndexEntry entry{};

        entry._header = (ObjectHeader*)*ptr;
        *ptr += sizeof(ObjectHeader);

        entry._filename = (char*)*ptr;
        *ptr += strlen(entry._filename) + 1;

        // decoded_chunk_size
        // ObjectHeader2* h2 = (ObjectHeader2*)ptr;
        *ptr += sizeof(ObjectHeader2);

        entry._name = (char*)*ptr;
        *ptr += strlen(entry._name) + 1;

        // ObjectHeader3* h3 = (ObjectHeader3*)ptr;
        *ptr += sizeof(ObjectHeader3);

        uint8_t* countA = (uint8_t*)*ptr;
        *ptr += sizeof(uint8_t);
        for (int n = 0; n < *countA; n++)
        {
            // header* subh = (header*)ptr;
            *ptr += sizeof(ObjectHeader);
        }

        uint8_t* countB = (uint8_t*)*ptr;
        *ptr += sizeof(uint8_t);
        for (int n = 0; n < *countB; n++)
        {
            // header* subh = (header*)ptr;
            *ptr += sizeof(ObjectHeader);
        }

        return entry;
    }
}
