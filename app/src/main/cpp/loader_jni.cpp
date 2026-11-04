// SPDX-License-Identifier: MPL-2.0
// Copyright © 2020 Skyline Team and Contributors (https://github.com/skyline-emu/)

#include "skyline/crypto/key_store.h"
#include "skyline/vfs/nca.h"
#include "skyline/vfs/os_backing.h"
#include "skyline/vfs/os_filesystem.h"
#include "skyline/loader/nro.h"
#include "skyline/loader/nso.h"
#include "skyline/loader/nca.h"
#include "skyline/loader/xci.h"
#include "skyline/loader/nsp.h"
#include "skyline/jvm.h"

#include "skyline/vfs/movie_extent.h"
#include "skyline/vfs/rom_filesystem.h"
#include "skyline/vfs/region_backing.h"
#include "skyline/services/fssrv/mp4_probe.h"

static std::unique_ptr<skyline::loader::Loader> OpenRom(JNIEnv *env, jint jformat, jint fd, jstring keyPath) {
    const auto format = static_cast<skyline::loader::RomFormat>(jformat);
    auto keyStore = std::make_shared<skyline::crypto::KeyStore>(skyline::JniString(env, keyPath));
    auto backing = std::make_shared<skyline::vfs::OsBacking>(fd);
    switch (format) {
        case skyline::loader::RomFormat::NRO:
            return std::make_unique<skyline::loader::NroLoader>(backing);
        case skyline::loader::RomFormat::NSO:
            return std::make_unique<skyline::loader::NsoLoader>(backing);
        case skyline::loader::RomFormat::NCA:
            return std::make_unique<skyline::loader::NcaLoader>(backing, keyStore);
        case skyline::loader::RomFormat::XCI:
            return std::make_unique<skyline::loader::XciLoader>(backing, keyStore);
        case skyline::loader::RomFormat::NSP:
            return std::make_unique<skyline::loader::NspLoader>(backing, keyStore);
        default:
            return {};
    }
}

extern "C" JNIEXPORT jint JNICALL Java_org_stratoemu_strato_loader_RomFile_populate(JNIEnv *env, jobject thiz, jint jformat, jint fd, jstring appFilesPathJstring, jint systemLanguage) {
    skyline::signal::ScopedStackBlocker stackBlocker;

    std::unique_ptr<skyline::loader::Loader> loader;
    try {
        loader = OpenRom(env, jformat, fd, appFilesPathJstring);
        if (!loader) return static_cast<jint>(skyline::loader::LoaderResult::ParsingError);
    } catch (const skyline::loader::loader_exception &e) {
        return static_cast<jint>(e.error);
    } catch (const std::exception &e) {
        return static_cast<jint>(skyline::loader::LoaderResult::ParsingError);
    }

    jclass clazz{env->GetObjectClass(thiz)};
    jfieldID applicationNameField{env->GetFieldID(clazz, "applicationName", "Ljava/lang/String;")};
    jfieldID applicationTitleIdField{env->GetFieldID(clazz, "applicationTitleId", "Ljava/lang/String;")};
    jfieldID applicationAuthorField{env->GetFieldID(clazz, "applicationAuthor", "Ljava/lang/String;")};
    jfieldID rawIconField{env->GetFieldID(clazz, "rawIcon", "[B")};
    jfieldID applicationVersionField{env->GetFieldID(clazz, "applicationVersion", "Ljava/lang/String;")};

    if (loader->nacp) {
        try {
            auto language{skyline::language::GetApplicationLanguage(static_cast<skyline::language::SystemLanguage>(systemLanguage))};
            if (((1 << static_cast<skyline::u32>(language)) & loader->nacp->supportedTitleLanguages) == 0)
                language = loader->nacp->GetFirstSupportedTitleLanguage();

            env->SetObjectField(thiz, applicationNameField, env->NewStringUTF(loader->nacp->GetApplicationName(language).c_str()));
            env->SetObjectField(thiz, applicationVersionField, env->NewStringUTF(loader->nacp->GetApplicationVersion().c_str()));
            env->SetObjectField(thiz, applicationTitleIdField, env->NewStringUTF(loader->nacp->GetSaveDataOwnerId().c_str()));
            env->SetObjectField(thiz, applicationAuthorField, env->NewStringUTF(loader->nacp->GetApplicationPublisher(language).c_str()));

            auto icon{loader->GetIcon(language)};
            jbyteArray iconByteArray{env->NewByteArray(static_cast<jsize>(icon.size()))};
            env->SetByteArrayRegion(iconByteArray, 0, static_cast<jsize>(icon.size()), reinterpret_cast<const jbyte *>(icon.data()));
            env->SetObjectField(thiz, rawIconField, iconByteArray);
        } catch (const std::exception &e) {
            // A malformed NACP/icon in a single ROM must not crash the whole app during a
            // library scan. A C++ exception that escapes a JNI-called function past this
            // point would call std::terminate() and kill the process with no catchable
            // Java-side error. Instead, report this ROM as unparseable and keep scanning.
            return static_cast<jint>(skyline::loader::LoaderResult::ParsingError);
        } catch (...) {
            return static_cast<jint>(skyline::loader::LoaderResult::ParsingError);
        }
    }

    return static_cast<jint>(skyline::loader::LoaderResult::Success);
}

extern "C" JNIEXPORT jstring Java_org_stratoemu_strato_preference_FirmwareImportPreference_fetchFirmwareVersion(JNIEnv *env, jobject thiz, jstring systemArchivesPathJstring, jstring keysPathJstring) {
    struct SystemVersion {
        skyline::u8 major;
        skyline::u8 minor;
        skyline::u8 micro;
        skyline::u8 _pad0_;
        skyline::u8 revisionMajor;
        skyline::u8 revisionMinor;
        skyline::u8 _pad1_[2];
        skyline::u8 platformString[0x20];
        skyline::u8 versionHash[0x40];
        skyline::u8 displayVersion[0x18];
        skyline::u8 displayTitle[0x80];
    };

    constexpr skyline::u64 systemVersionProgramId{0x0100000000000809};

    auto systemArchivesFileSystem{std::make_shared<skyline::vfs::OsFileSystem>(skyline::JniString(env, systemArchivesPathJstring))};
    auto systemArchives{systemArchivesFileSystem->OpenDirectory("")};
    auto keyStore{std::make_shared<skyline::crypto::KeyStore>(skyline::JniString(env, keysPathJstring))};

    for (const auto &entry : systemArchives->Read()) {
        try {
            std::shared_ptr<skyline::vfs::Backing> backing{systemArchivesFileSystem->OpenFile(entry.name)};
            auto nca{skyline::vfs::NCA(backing, keyStore)};

            if (nca.header.programId == systemVersionProgramId && nca.romFs != nullptr) {
                auto controlRomFs{std::make_shared<skyline::vfs::RomFileSystem>(nca.romFs)};
                auto file{controlRomFs->OpenFile("file")};
                SystemVersion systemVersion;
                file->Read<SystemVersion>(systemVersion);
                return env->NewStringUTF(reinterpret_cast<char *>(systemVersion.displayVersion));
            }
        } catch (const std::exception &e) {
            // Skip NCAs that cannot be parsed or decrypted with the current key set.
            // This is expected for NCAs encrypted with key generations that are higher
            // than what the prod.keys supports, or for corrupted/partial firmware files.
            continue;
        } catch (...) {
            continue;
        }
    }

    return env->NewStringUTF("");
}

/**
 * @brief Names of the NCAs that are enough to run games, in the order they were found
 * @note These are the only archives the emulator reads on its own: the system version (0x...809) and the shared fonts (0x...810 to 0x...814).
 * Games can also ask for other system data archives (Mii models 0x...802, time zones 0x...80E, certificates 0x...800...), add their IDs here if one needs them
 */
extern "C" JNIEXPORT jobjectArray Java_org_stratoemu_strato_preference_FirmwareImportPreference_listEssentialArchives(JNIEnv *env, jobject thiz, jstring systemArchivesPathJstring, jstring keysPathJstring) {
    constexpr skyline::u64 firstEssentialProgramId{0x0100000000000809};
    constexpr skyline::u64 lastEssentialProgramId{0x0100000000000814};

    auto systemArchivesFileSystem{std::make_shared<skyline::vfs::OsFileSystem>(skyline::JniString(env, systemArchivesPathJstring))};
    auto systemArchives{systemArchivesFileSystem->OpenDirectory("")};
    auto keyStore{std::make_shared<skyline::crypto::KeyStore>(skyline::JniString(env, keysPathJstring))};

    std::vector<std::string> names;
    for (const auto &entry : systemArchives->Read()) {
        try {
            std::shared_ptr<skyline::vfs::Backing> backing{systemArchivesFileSystem->OpenFile(entry.name)};
            auto nca{skyline::vfs::NCA(backing, keyStore)};

            // 0x80A to 0x80F are other system archives (avatars, news, time zones...) that sit between the version and the fonts
            bool essential{nca.header.programId == firstEssentialProgramId || (nca.header.programId >= 0x0100000000000810 && nca.header.programId <= lastEssentialProgramId)};
            if (essential && nca.romFs != nullptr)
                names.push_back(entry.name);
        } catch (...) {
            continue; // Not parsable with the current keys, so it can't be used by the emulator either
        }
    }

    jobjectArray result{env->NewObjectArray(static_cast<jsize>(names.size()), env->FindClass("java/lang/String"), nullptr)};
    for (size_t i{}; i < names.size(); i++)
        env->SetObjectArrayElement(result, static_cast<jsize>(i), env->NewStringUTF(names[i].c_str()));
    return result;
}

std::vector<skyline::u8> decodeBfttfFont(const std::shared_ptr<skyline::vfs::Backing> bfttfFile){
    constexpr skyline::u32 fontKey{0x06186249};
    constexpr skyline::u32 BFTTFMagic{0x18029a7f};

    auto firstBytes{bfttfFile->Read<skyline::u32>()};
    auto firstBytesXor{firstBytes ^ fontKey};

    if (firstBytesXor == BFTTFMagic) {
        constexpr size_t initialOffset{8};
        std::vector<skyline::u8> font(bfttfFile->size - initialOffset);

        for (size_t offset = initialOffset; offset < bfttfFile->size; offset += 4) {
            skyline::u32 decodedData{bfttfFile->Read<skyline::u32>(offset) ^ fontKey};

            font[offset - 8] = static_cast<skyline::u8>(decodedData >> 0);
            font[offset - 7] = static_cast<skyline::u8>(decodedData >> 8);
            font[offset - 6] = static_cast<skyline::u8>(decodedData >> 16);
            font[offset - 5] = static_cast<skyline::u8>(decodedData >> 24);
        }

        return font;
    }
    return {};
}

extern "C" JNIEXPORT void Java_org_stratoemu_strato_preference_FirmwareImportPreference_extractFonts(JNIEnv *env, jobject thiz, jstring systemArchivesPathJstring, jstring keysPathJstring, jstring fontsPath) {
    // Fonts are stored in the following NCAs
    // 0x0100000000000810 -> "FontNintendoExtended"
    // 0x0100000000000811 -> "FontStandard"
    // 0x0100000000000812 -> "FontKorean"
    // 0x0100000000000813 -> "FontChineseTraditional"
    // 0x0100000000000814 -> "FontChineseSimplified"

    constexpr skyline::u64 firstFontProgramId{0x0100000000000810};
    constexpr skyline::u64 lastFontProgramId{0x0100000000000814};

    const std::map<std::string, std::string> sharedFontFilenameDictionary = {
        {"nintendo_ext_003.bfttf", "FontNintendoExtended"},
        {"nintendo_ext2_003.bfttf", "FontNintendoExtended2"},
        {"nintendo_udsg-r_std_003.bfttf", "FontStandard"},
        {"nintendo_udsg-r_ko_003.bfttf", "FontKorean"},
        {"nintendo_udjxh-db_zh-tw_003.bfttf", "FontChineseTraditional"},
        {"nintendo_udsg-r_org_zh-cn_003.bfttf", "FontChineseSimplified"},
        {"nintendo_udsg-r_ext_zh-cn_003.bfttf", "FontExtendedChineseSimplified"}
    };

    auto fontsFileSystem{std::make_shared<skyline::vfs::OsFileSystem>(skyline::JniString(env, fontsPath))};
    auto systemArchivesFileSystem{std::make_shared<skyline::vfs::OsFileSystem>(skyline::JniString(env, systemArchivesPathJstring))};
    auto systemArchives{systemArchivesFileSystem->OpenDirectory("")};
    auto keyStore{std::make_shared<skyline::crypto::KeyStore>(skyline::JniString(env, keysPathJstring))};

    for (const auto &entry : systemArchives->Read()) {
        std::shared_ptr<skyline::vfs::Backing> backing{systemArchivesFileSystem->OpenFile(entry.name)};
        auto nca{skyline::vfs::NCA(backing, keyStore)};

        if (nca.header.programId >= firstFontProgramId && nca.header.programId <= lastFontProgramId && nca.romFs != nullptr) {
            auto controlRomFs{std::make_shared<skyline::vfs::RomFileSystem>(nca.romFs)};

            for (auto fileEntry = controlRomFs->fileMap.begin(); fileEntry != controlRomFs->fileMap.end(); fileEntry++) {
                auto fileName{fileEntry->first};
                auto bfttfFile{controlRomFs->OpenFile(fileName)};

                auto decodedFont{decodeBfttfFont(bfttfFile)};
                if (decodedFont.empty())
                    continue;

                auto ttfFileName{sharedFontFilenameDictionary.at(fileName) + ".ttf"};
                if (fontsFileSystem->FileExists(ttfFileName))
                    fontsFileSystem->DeleteFile(ttfFileName);

                fontsFileSystem->CreateFile(ttfFileName, decodedFont.size());
                std::shared_ptr<skyline::vfs::Backing> ttfFile{fontsFileSystem->OpenFile(ttfFileName, {true, true, false})};

                ttfFile->Write(decodedFont);
            }
        }
    }
}
// The Kotlin owner holds an independently opened ROM fd until the last read returns.
namespace {
struct MovieReader {
    std::shared_ptr<skyline::vfs::RegionBacking> file;
    std::vector<skyline::u8> buffer;
};
}

extern "C" JNIEXPORT jlong JNICALL Java_org_stratoemu_strato_emulation_RomfsMovie_openNative(
        JNIEnv *env, jobject, jint fd, jint format, jstring keys, jlong offset) {
    try {
        if (offset < 0) return 0;
        auto loader = OpenRom(env, format, fd, keys);
        if (!loader || !loader->romFs) return 0;
        const auto &rom = loader->romFs;
        const auto h = rom->Read<skyline::vfs::RomFileSystem::RomFsHeader>();
        constexpr size_t maxTable = 8 * 1024 * 1024;
        const auto pos = static_cast<std::uint64_t>(offset);
        if (h.headerSize != sizeof(h) || h.dataOffset < sizeof(h) || h.dataOffset > rom->size ||
            pos < h.dataOffset || !h.fileMetaTableSize || h.fileMetaTableSize > maxTable)
            return 0;
        // RomFS images can put the hash/metadata tables after the file partition.
        std::uint64_t end = rom->size;
        for (const auto &[start, length] : {std::pair{h.dirHashTableOffset, h.dirHashTableSize},
                std::pair{h.dirMetaTableOffset, h.dirMetaTableSize}, std::pair{h.fileHashTableOffset, h.fileHashTableSize},
                std::pair{h.fileMetaTableOffset, h.fileMetaTableSize}}) {
            if (!length) continue;
            if (start < sizeof(h) || start > rom->size || length > rom->size - start) return 0;
            if (start >= h.dataOffset) end = std::min<std::uint64_t>(end, start);
            else if (length > h.dataOffset - start) return 0;
        }
        std::vector<skyline::u8> table(h.fileMetaTableSize);
        rom->Read(skyline::span(table), h.fileMetaTableOffset);
        const auto size = skyline::vfs::MovieExtent(table, end - h.dataOffset, pos - h.dataOffset);
        if (!size || *size < 16) return 0;
        auto movie = std::make_shared<skyline::vfs::RegionBacking>(rom, pos, *size);
        std::array<skyline::u8, 256> header{};
        auto prefix = skyline::span<skyline::u8>(header.data(), std::min(movie->size, header.size()));
        movie->Read(prefix);
        if (!skyline::service::fssrv::IsMp4Candidate(prefix)) return 0;
        return reinterpret_cast<jlong>(new MovieReader{std::move(movie), {}});
    } catch (...) { return 0; }
}

extern "C" JNIEXPORT jlong JNICALL Java_org_stratoemu_strato_emulation_RomfsMovie_sizeNative(
        JNIEnv *, jobject, jlong handle) {
    return static_cast<jlong>(reinterpret_cast<MovieReader *>(handle)->file->size);
}

extern "C" JNIEXPORT jint JNICALL Java_org_stratoemu_strato_emulation_RomfsMovie_readNative(
        JNIEnv *env, jobject, jlong handle, jlong pos, jbyteArray out, jint off, jint len) {
    try {
        auto &reader = *reinterpret_cast<MovieReader *>(handle);
        if (pos < 0 || !out || off < 0 || len < 0 || off > env->GetArrayLength(out) - len)
            return -2;
        if (!len) return 0;
        if (static_cast<size_t>(pos) >= reader.file->size) return -1;
        const auto count = std::min({reader.file->size - static_cast<size_t>(pos),
                                     static_cast<size_t>(len), size_t{64 * 1024}});
        reader.buffer.resize(count);
        reader.file->Read(skyline::span(reader.buffer), static_cast<size_t>(pos));
        env->SetByteArrayRegion(out, off, static_cast<jsize>(count), reinterpret_cast<const jbyte *>(reader.buffer.data()));
        return static_cast<jint>(count);
    } catch (...) { return -2; }
}

extern "C" JNIEXPORT void JNICALL Java_org_stratoemu_strato_emulation_RomfsMovie_closeNative(
        JNIEnv *, jobject, jlong handle) {
    delete reinterpret_cast<MovieReader *>(handle);
}
