/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file Copyright.txt or https://cmake.org/licensing for details.  */
#include "cmQtAutoGen.h"
#include "cmQtAutoGeneratorMocUic.h"

#include <algorithm>
#include <array>
#include <list>
#include <memory>
#include <sstream>
#include <string.h>
#include <utility>

#include "cmAlgorithms.h"
#include "cmCryptoHash.h"
#include "cmMakefile.h"
#include "cmOutputConverter.h"
#include "cmSystemTools.h"
#include "cmake.h"

#if defined(__APPLE__)
#include <unistd.h>
#endif

// -- Static variables

static const char* SettingsKeyMoc = "AM_MOC_SETTINGS_HASH";
static const char* SettingsKeyUic = "AM_UIC_SETTINGS_HASH";

// -- Static functions

static std::string SubDirPrefix(std::string const& fileName)
{
  std::string res(cmSystemTools::GetFilenamePath(fileName));
  if (!res.empty()) {
    res += '/';
  }
  return res;
}

static bool ListContains(std::vector<std::string> const& list,
                         std::string const& entry)
{
  return (std::find(list.begin(), list.end(), entry) != list.end());
}

// -- Class methods

cmQtAutoGeneratorMocUic::cmQtAutoGeneratorMocUic()
  : MultiConfig(cmQtAutoGen::WRAP)
  , IncludeProjectDirsBefore(false)
  , QtVersionMajor(4)
  , MocSettingsChanged(false)
  , MocPredefsChanged(false)
  , MocRelaxedMode(false)
  , UicSettingsChanged(false)
{
  // Precompile regular expressions
  this->MocRegExpInclude.compile(
    "[\n][ \t]*#[ \t]*include[ \t]+"
    "[\"<](([^ \">]+/)?moc_[^ \">/]+\\.cpp|[^ \">]+\\.moc)[\">]");
  this->UicRegExpInclude.compile("[\n][ \t]*#[ \t]*include[ \t]+"
                                 "[\"<](([^ \">]+/)?ui_[^ \">/]+\\.h)[\">]");
}

bool cmQtAutoGeneratorMocUic::InitInfoFile(cmMakefile* makefile)
{
  // -- Meta
  this->HeaderExtensions = makefile->GetCMakeInstance()->GetHeaderExtensions();

  // Utility lambdas
  auto InfoGet = [makefile](const char* key) {
    return makefile->GetSafeDefinition(key);
  };
  auto InfoGetBool = [makefile](const char* key) {
    return makefile->IsOn(key);
  };
  auto InfoGetList = [makefile](const char* key) -> std::vector<std::string> {
    std::vector<std::string> list;
    cmSystemTools::ExpandListArgument(makefile->GetSafeDefinition(key), list);
    return list;
  };
  auto InfoGetLists =
    [makefile](const char* key) -> std::vector<std::vector<std::string>> {
    std::vector<std::vector<std::string>> lists;
    {
      std::string const value = makefile->GetSafeDefinition(key);
      std::string::size_type pos = 0;
      while (pos < value.size()) {
        std::string::size_type next = value.find(cmQtAutoGen::listSep, pos);
        std::string::size_type length =
          (next != std::string::npos) ? next - pos : value.size() - pos;
        // Remove enclosing braces
        if (length >= 2) {
          std::string::const_iterator itBeg = value.begin() + (pos + 1);
          std::string::const_iterator itEnd = itBeg + (length - 2);
          {
            std::string subValue(itBeg, itEnd);
            std::vector<std::string> list;
            cmSystemTools::ExpandListArgument(subValue, list);
            lists.push_back(std::move(list));
          }
        }
        pos += length;
        pos += cmQtAutoGen::listSep.size();
      }
    }
    return lists;
  };
  auto InfoGetConfig = [makefile, this](const char* key) -> std::string {
    const char* valueConf = nullptr;
    {
      std::string keyConf = key;
      keyConf += '_';
      keyConf += this->GetInfoConfig();
      valueConf = makefile->GetDefinition(keyConf);
    }
    if (valueConf == nullptr) {
      valueConf = makefile->GetSafeDefinition(key);
    }
    return std::string(valueConf);
  };
  auto InfoGetConfigList =
    [&InfoGetConfig](const char* key) -> std::vector<std::string> {
    std::vector<std::string> list;
    cmSystemTools::ExpandListArgument(InfoGetConfig(key), list);
    return list;
  };

  // -- Read info file
  if (!makefile->ReadListFile(this->GetInfoFile().c_str())) {
    this->LogFileError(cmQtAutoGen::GEN, this->GetInfoFile(),
                       "File processing failed");
    return false;
  }

  // -- Meta
  this->MultiConfig = cmQtAutoGen::MultiConfigType(InfoGet("AM_MULTI_CONFIG"));
  this->ConfigSuffix = InfoGetConfig("AM_CONFIG_SUFFIX");
  if (this->ConfigSuffix.empty()) {
    this->ConfigSuffix = "_";
    this->ConfigSuffix += this->GetInfoConfig();
  }

  this->SettingsFile = InfoGetConfig("AM_SETTINGS_FILE");
  if (this->SettingsFile.empty()) {
    this->LogFileError(cmQtAutoGen::GEN, this->GetInfoFile(),
                       "Settings file name missing");
    return false;
  }

  // - Files and directories
  this->ProjectSourceDir = InfoGet("AM_CMAKE_SOURCE_DIR");
  this->ProjectBinaryDir = InfoGet("AM_CMAKE_BINARY_DIR");
  this->CurrentSourceDir = InfoGet("AM_CMAKE_CURRENT_SOURCE_DIR");
  this->CurrentBinaryDir = InfoGet("AM_CMAKE_CURRENT_BINARY_DIR");
  this->IncludeProjectDirsBefore =
    InfoGetBool("AM_CMAKE_INCLUDE_DIRECTORIES_PROJECT_BEFORE");
  this->AutogenBuildDir = InfoGet("AM_BUILD_DIR");
  if (this->AutogenBuildDir.empty()) {
    this->LogFileError(cmQtAutoGen::GEN, this->GetInfoFile(),
                       "Autogen build directory missing");
    return false;
  }

  // - Qt environment
  if (!cmSystemTools::StringToULong(InfoGet("AM_QT_VERSION_MAJOR"),
                                    &this->QtVersionMajor)) {
    this->QtVersionMajor = 4;
  }
  this->MocExecutable = InfoGet("AM_QT_MOC_EXECUTABLE");
  this->UicExecutable = InfoGet("AM_QT_UIC_EXECUTABLE");

  // - Moc
  if (this->MocEnabled()) {
    this->MocSkipList = InfoGetList("AM_MOC_SKIP");
    this->MocDefinitions = InfoGetConfigList("AM_MOC_DEFINITIONS");
#ifdef _WIN32
    {
      std::string const win32("WIN32");
      if (!ListContains(this->MocDefinitions, win32)) {
        this->MocDefinitions.push_back(win32);
      }
    }
#endif
    this->MocIncludePaths = InfoGetConfigList("AM_MOC_INCLUDES");
    this->MocOptions = InfoGetList("AM_MOC_OPTIONS");
    this->MocRelaxedMode = InfoGetBool("AM_MOC_RELAXED_MODE");
    {
      std::vector<std::string> const MocMacroNames =
        InfoGetList("AM_MOC_MACRO_NAMES");
      for (std::string const& item : MocMacroNames) {
        this->MocMacroFilters.emplace_back(
          item, ("[\n][ \t]*{?[ \t]*" + item).append("[^a-zA-Z0-9_]"));
      }
    }
    {
      std::vector<std::string> const mocDependFilters =
        InfoGetList("AM_MOC_DEPEND_FILTERS");
      // Insert Q_PLUGIN_METADATA dependency filter
      if (this->QtVersionMajor != 4) {
        this->MocDependFilterPush("Q_PLUGIN_METADATA",
                                  "[\n][ \t]*Q_PLUGIN_METADATA[ \t]*\\("
                                  "[^\\)]*FILE[ \t]*\"([^\"]+)\"");
      }
      // Insert user defined dependency filters
      if ((mocDependFilters.size() % 2) == 0) {
        for (std::vector<std::string>::const_iterator
               dit = mocDependFilters.begin(),
               ditEnd = mocDependFilters.end();
             dit != ditEnd; dit += 2) {
          if (!this->MocDependFilterPush(*dit, *(dit + 1))) {
            return false;
          }
        }
      } else {
        this->LogFileError(
          cmQtAutoGen::MOC, this->GetInfoFile(),
          "AUTOMOC_DEPEND_FILTERS list size is not a multiple of 2");
        return false;
      }
    }
    this->MocPredefsCmd = InfoGetList("AM_MOC_PREDEFS_CMD");
  }

  // - Uic
  if (this->UicEnabled()) {
    this->UicSkipList = InfoGetList("AM_UIC_SKIP");
    this->UicSearchPaths = InfoGetList("AM_UIC_SEARCH_PATHS");
    this->UicTargetOptions = InfoGetConfigList("AM_UIC_TARGET_OPTIONS");
    {
      auto sources = InfoGetList("AM_UIC_OPTIONS_FILES");
      auto options = InfoGetLists("AM_UIC_OPTIONS_OPTIONS");
      // Compare list sizes
      if (sources.size() != options.size()) {
        std::ostringstream ost;
        ost << "files/options lists sizes mismatch (" << sources.size() << "/"
            << options.size() << ")";
        this->LogFileError(cmQtAutoGen::UIC, this->GetInfoFile(), ost.str());
        return false;
      }
      auto fitEnd = sources.cend();
      auto fit = sources.begin();
      auto oit = options.begin();
      while (fit != fitEnd) {
        this->UicOptions[*fit] = std::move(*oit);
        ++fit;
        ++oit;
      }
    }
  }

  // Initialize source file jobs
  {
    // Utility lambdas
    auto AddJob = [this](std::map<std::string, SourceJob>& jobs,
                         std::string&& sourceFile) {
      const bool moc = !this->MocSkip(sourceFile);
      const bool uic = !this->UicSkip(sourceFile);
      if (moc || uic) {
        SourceJob& job = jobs[std::move(sourceFile)];
        job.Moc = moc;
        job.Uic = uic;
      }
    };

    // Add header jobs
    for (std::string& hdr : InfoGetList("AM_HEADERS")) {
      AddJob(this->HeaderJobs, std::move(hdr));
    }
    // Add source jobs
    {
      std::vector<std::string> sources = InfoGetList("AM_SOURCES");
      // Add header(s) for the source file
      for (std::string const& src : sources) {
        const bool srcMoc = !this->MocSkip(src);
        const bool srcUic = !this->UicSkip(src);
        if (!srcMoc && !srcUic) {
          continue;
        }
        // Search for the default header file and a private header
        std::array<std::string, 2> headerBases;
        headerBases[0] = SubDirPrefix(src);
        headerBases[0] += cmSystemTools::GetFilenameWithoutLastExtension(src);
        headerBases[1] = headerBases[0];
        headerBases[1] += "_p";
        for (std::string const& headerBase : headerBases) {
          std::string header;
          if (this->FindHeader(header, headerBase)) {
            const bool moc = srcMoc && !this->MocSkip(header);
            const bool uic = srcUic && !this->UicSkip(header);
            if (moc || uic) {
              SourceJob& job = this->HeaderJobs[std::move(header)];
              job.Moc = moc;
              job.Uic = uic;
            }
          }
        }
      }
      // Add Source jobs
      for (std::string& src : sources) {
        AddJob(this->SourceJobs, std::move(src));
      }
    }
  }

  // Init derived information
  // ------------------------

  // Init file path checksum generator
  this->FilePathChecksum.setupParentDirs(
    this->CurrentSourceDir, this->CurrentBinaryDir, this->ProjectSourceDir,
    this->ProjectBinaryDir);

  // include directory
  this->AutogenIncludeDir = "include";
  if (this->MultiConfig != cmQtAutoGen::SINGLE) {
    this->AutogenIncludeDir += this->ConfigSuffix;
  }
  this->AutogenIncludeDir += "/";

  // Moc variables
  if (this->MocEnabled()) {
    // Mocs compilation file
    this->MocCompFileRel = "mocs_compilation";
    if (this->MultiConfig == cmQtAutoGen::FULL) {
      this->MocCompFileRel += this->ConfigSuffix;
    }
    this->MocCompFileRel += ".cpp";
    this->MocCompFileAbs = cmSystemTools::CollapseCombinedPath(
      this->AutogenBuildDir, this->MocCompFileRel);

    // Moc predefs file
    if (!this->MocPredefsCmd.empty()) {
      this->MocPredefsFileRel = "moc_predefs";
      if (this->MultiConfig != cmQtAutoGen::SINGLE) {
        this->MocPredefsFileRel += this->ConfigSuffix;
      }
      this->MocPredefsFileRel += ".h";
      this->MocPredefsFileAbs = cmSystemTools::CollapseCombinedPath(
        this->AutogenBuildDir, this->MocPredefsFileRel);
    }

    // Sort include directories on demand
    if (this->IncludeProjectDirsBefore) {
      // Move strings to temporary list
      std::list<std::string> includes;
      includes.insert(includes.end(), this->MocIncludePaths.begin(),
                      this->MocIncludePaths.end());
      this->MocIncludePaths.clear();
      this->MocIncludePaths.reserve(includes.size());
      // Append project directories only
      {
        std::array<std::string const*, 2> const movePaths = {
          { &this->ProjectBinaryDir, &this->ProjectSourceDir }
        };
        for (std::string const* ppath : movePaths) {
          std::list<std::string>::iterator it = includes.begin();
          while (it != includes.end()) {
            std::string const& path = *it;
            if (cmSystemTools::StringStartsWith(path, ppath->c_str())) {
              this->MocIncludePaths.push_back(path);
              it = includes.erase(it);
            } else {
              ++it;
            }
          }
        }
      }
      // Append remaining directories
      this->MocIncludePaths.insert(this->MocIncludePaths.end(),
                                   includes.begin(), includes.end());
    }
    // Compose moc includes list
    {
      std::set<std::string> frameworkPaths;
      for (std::string const& path : this->MocIncludePaths) {
        this->MocIncludes.push_back("-I" + path);
        // Extract framework path
        if (cmHasLiteralSuffix(path, ".framework/Headers")) {
          // Go up twice to get to the framework root
          std::vector<std::string> pathComponents;
          cmSystemTools::SplitPath(path, pathComponents);
          std::string frameworkPath = cmSystemTools::JoinPath(
            pathComponents.begin(), pathComponents.end() - 2);
          frameworkPaths.insert(frameworkPath);
        }
      }
      // Append framework includes
      for (std::string const& path : frameworkPaths) {
        this->MocIncludes.push_back("-F");
        this->MocIncludes.push_back(path);
      }
    }
    // Setup single list with all options
    {
      // Add includes
      this->MocAllOptions.insert(this->MocAllOptions.end(),
                                 this->MocIncludes.begin(),
                                 this->MocIncludes.end());
      // Add definitions
      for (std::string const& def : this->MocDefinitions) {
        this->MocAllOptions.push_back("-D" + def);
      }
      // Add options
      this->MocAllOptions.insert(this->MocAllOptions.end(),
                                 this->MocOptions.begin(),
                                 this->MocOptions.end());
    }
  }

  return true;
}

void cmQtAutoGeneratorMocUic::SettingsFileRead(cmMakefile* makefile)
{
  // Compose current settings strings
  {
    cmCryptoHash crypt(cmCryptoHash::AlgoSHA256);
    std::string const sep(" ~~~ ");
    if (this->MocEnabled()) {
      std::string str;
      str += this->MocExecutable;
      str += sep;
      str += cmJoin(this->MocAllOptions, ";");
      str += sep;
      str += this->IncludeProjectDirsBefore ? "TRUE" : "FALSE";
      str += sep;
      str += cmJoin(this->MocPredefsCmd, ";");
      str += sep;
      this->SettingsStringMoc = crypt.HashString(str);
    }
    if (this->UicEnabled()) {
      std::string str;
      str += this->UicExecutable;
      str += sep;
      str += cmJoin(this->UicTargetOptions, ";");
      for (const auto& item : this->UicOptions) {
        str += sep;
        str += item.first;
        str += sep;
        str += cmJoin(item.second, ";");
      }
      str += sep;
      this->SettingsStringUic = crypt.HashString(str);
    }
  }

  // Read old settings
  if (makefile->ReadListFile(this->SettingsFile.c_str())) {
    {
      auto SMatch = [makefile](const char* key, std::string const& value) {
        return (value == makefile->GetSafeDefinition(key));
      };
      if (!SMatch(SettingsKeyMoc, this->SettingsStringMoc)) {
        this->MocSettingsChanged = true;
      }
      if (!SMatch(SettingsKeyUic, this->SettingsStringUic)) {
        this->UicSettingsChanged = true;
      }
    }
    // In case any setting changed remove the old settings file.
    // This triggers a full rebuild on the next run if the current
    // build is aborted before writing the current settings in the end.
    if (this->SettingsChanged()) {
      cmSystemTools::RemoveFile(this->SettingsFile);
    }
  } else {
    // If the file could not be read re-generate everythiung.
    this->MocSettingsChanged = true;
    this->UicSettingsChanged = true;
  }
}

bool cmQtAutoGeneratorMocUic::SettingsFileWrite()
{
  bool success = true;
  // Only write if any setting changed
  if (this->SettingsChanged()) {
    if (this->GetVerbose()) {
      this->LogInfo(cmQtAutoGen::GEN, "Writing settings file " +
                      cmQtAutoGen::Quoted(this->SettingsFile));
    }
    // Compose settings file content
    std::string settings;
    {
      auto SettingAppend = [&settings](const char* key,
                                       std::string const& value) {
        settings += "set(";
        settings += key;
        settings += " ";
        settings += cmOutputConverter::EscapeForCMake(value);
        settings += ")\n";
      };
      SettingAppend(SettingsKeyMoc, this->SettingsStringMoc);
      SettingAppend(SettingsKeyUic, this->SettingsStringUic);
    }
    // Write settings file
    if (!this->FileWrite(cmQtAutoGen::GEN, this->SettingsFile, settings)) {
      this->LogFileError(cmQtAutoGen::GEN, this->SettingsFile,
                         "Settings file writing failed");
      // Remove old settings file to trigger a full rebuild on the next run
      cmSystemTools::RemoveFile(this->SettingsFile);
      success = false;
    }
  }
  return success;
}

bool cmQtAutoGeneratorMocUic::Process(cmMakefile* makefile)
{
  // the program goes through all .cpp files to see which moc files are
  // included. It is not really interesting how the moc file is named, but
  // what file the moc is created from. Once a moc is included the same moc
  // may not be included in the mocs_compilation.cpp file anymore.
  // OTOH if there's a header containing Q_OBJECT where no corresponding
  // moc file is included anywhere a moc_<filename>.cpp file is created and
  // included in the mocs_compilation.cpp file.

  if (!this->InitInfoFile(makefile)) {
    return false;
  }
  // Read latest settings
  this->SettingsFileRead(makefile);

  // Create AUTOGEN include directory
  {
    std::string const incDirAbs = cmSystemTools::CollapseCombinedPath(
      this->AutogenBuildDir, this->AutogenIncludeDir);
    if (!cmSystemTools::MakeDirectory(incDirAbs)) {
      this->LogFileError(cmQtAutoGen::GEN, incDirAbs,
                         "Could not create directory");
      return false;
    }
  }

  // Parse source files
  for (const auto& item : this->SourceJobs) {
    if (!this->ParseSourceFile(item.first, item.second)) {
      return false;
    }
  }
  // Parse header files
  for (const auto& item : this->HeaderJobs) {
    if (!this->ParseHeaderFile(item.first, item.second)) {
      return false;
    }
  }
  // Read missing dependency information
  if (!this->ParsePostprocess()) {
    return false;
  }

  // Generate files
  if (!this->MocGenerateAll()) {
    return false;
  }
  if (!this->UicGenerateAll()) {
    return false;
  }

  if (!this->SettingsFileWrite()) {
    return false;
  }

  return true;
}

/**
 * @return True on success
 */
bool cmQtAutoGeneratorMocUic::ParseSourceFile(std::string const& absFilename,
                                              const SourceJob& job)
{
  std::string contentText;
  std::string error;
  bool success = this->FileRead(contentText, absFilename, &error);
  if (success) {
    if (!contentText.empty()) {
      if (job.Moc) {
        success = this->MocParseSourceContent(absFilename, contentText);
      }
      if (success && job.Uic) {
        success = this->UicParseContent(absFilename, contentText);
      }
    } else {
      this->LogFileWarning(cmQtAutoGen::GEN, absFilename,
                           "The source file is empty");
    }
  } else {
    this->LogFileError(cmQtAutoGen::GEN, absFilename,
                       "Could not read the source file: " + error);
  }
  return success;
}

/**
 * @return True on success
 */
bool cmQtAutoGeneratorMocUic::ParseHeaderFile(std::string const& absFilename,
                                              const SourceJob& job)
{
  std::string contentText;
  std::string error;
  bool success = this->FileRead(contentText, absFilename, &error);
  if (success) {
    if (!contentText.empty()) {
      if (job.Moc) {
        this->MocParseHeaderContent(absFilename, contentText);
      }
      if (job.Uic) {
        success = this->UicParseContent(absFilename, contentText);
      }
    } else {
      this->LogFileWarning(cmQtAutoGen::GEN, absFilename,
                           "The header file is empty");
    }
  } else {
    this->LogFileError(cmQtAutoGen::GEN, absFilename,
                       "Could not read the header file: " + error);
  }
  return success;
}

/**
 * @return True on success
 */
bool cmQtAutoGeneratorMocUic::ParsePostprocess()
{
  bool success = true;
  // Read missing dependencies
  for (auto& item : this->MocJobsIncluded) {
    if (!item->DependsValid) {
      std::string content;
      std::string error;
      if (this->FileRead(content, item->SourceFile, &error)) {
        this->MocFindDepends(item->SourceFile, content, item->Depends);
        item->DependsValid = true;
      } else {
        std::string emsg = "Could not read file\n  ";
        emsg += item->SourceFile;
        emsg += "\nrequired by moc include \"";
        emsg += item->IncludeString;
        emsg += "\".\n";
        emsg += error;
        this->LogFileError(cmQtAutoGen::MOC, item->Includer, emsg);
        success = false;
        break;
      }
    }
  }
  return success;
}

/**
 * @brief Tests if the file should be ignored for moc scanning
 * @return True if the file should be ignored
 */
bool cmQtAutoGeneratorMocUic::MocSkip(std::string const& absFilename) const
{
  if (this->MocEnabled()) {
    // Test if the file name is on the skip list
    if (!ListContains(this->MocSkipList, absFilename)) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Tests if the C++ content requires moc processing
 * @return True if moc is required
 */
bool cmQtAutoGeneratorMocUic::MocRequired(std::string const& contentText,
                                          std::string* macroName)
{
  for (KeyRegExp& filter : this->MocMacroFilters) {
    // Run a simple find string operation before the expensive
    // regular expression check
    if (contentText.find(filter.Key) != std::string::npos) {
      if (filter.RegExp.find(contentText)) {
        // Return macro name on demand
        if (macroName != nullptr) {
          *macroName = filter.Key;
        }
        return true;
      }
    }
  }
  return false;
}

std::string cmQtAutoGeneratorMocUic::MocStringMacros() const
{
  std::string res;
  const auto itB = this->MocMacroFilters.cbegin();
  const auto itE = this->MocMacroFilters.cend();
  const auto itL = itE - 1;
  auto itC = itB;
  for (; itC != itE; ++itC) {
    // Separator
    if (itC != itB) {
      if (itC != itL) {
        res += ", ";
      } else {
        res += " or ";
      }
    }
    // Key
    res += itC->Key;
  }
  return res;
}

std::string cmQtAutoGeneratorMocUic::MocStringHeaders(
  std::string const& fileBase) const
{
  std::string res = fileBase;
  res += ".{";
  res += cmJoin(this->HeaderExtensions, ",");
  res += "}";
  return res;
}

std::string cmQtAutoGeneratorMocUic::MocFindIncludedHeader(
  std::string const& sourcePath, std::string const& includeBase) const
{
  std::string header;
  // Search in vicinity of the source
  if (!this->FindHeader(header, sourcePath + includeBase)) {
    // Search in include directories
    for (std::string const& path : this->MocIncludePaths) {
      std::string fullPath = path;
      fullPath.push_back('/');
      fullPath += includeBase;
      if (this->FindHeader(header, fullPath)) {
        break;
      }
    }
  }
  // Sanitize
  if (!header.empty()) {
    header = cmSystemTools::GetRealPath(header);
  }
  return header;
}

bool cmQtAutoGeneratorMocUic::MocFindIncludedFile(
  std::string& absFile, std::string const& sourcePath,
  std::string const& includeString) const
{
  bool success = false;
  // Search in vicinity of the source
  {
    std::string testPath = sourcePath;
    testPath += includeString;
    if (cmSystemTools::FileExists(testPath.c_str())) {
      absFile = cmSystemTools::GetRealPath(testPath);
      success = true;
    }
  }
  // Search in include directories
  if (!success) {
    for (std::string const& path : this->MocIncludePaths) {
      std::string fullPath = path;
      fullPath.push_back('/');
      fullPath += includeString;
      if (cmSystemTools::FileExists(fullPath.c_str())) {
        absFile = cmSystemTools::GetRealPath(fullPath);
        success = true;
        break;
      }
    }
  }
  return success;
}

bool cmQtAutoGeneratorMocUic::MocDependFilterPush(std::string const& key,
                                                  std::string const& regExp)
{
  std::string error;
  if (!key.empty()) {
    if (!regExp.empty()) {
      KeyRegExp filter;
      filter.Key = key;
      if (filter.RegExp.compile(regExp)) {
        this->MocDependFilters.push_back(std::move(filter));
      } else {
        error = "Regular expression compiling failed";
      }
    } else {
      error = "Regular expression is empty";
    }
  } else {
    error = "Key is empty";
  }
  if (!error.empty()) {
    std::string emsg = "AUTOMOC_DEPEND_FILTERS: ";
    emsg += error;
    emsg += "\n";
    emsg += "  Key:    ";
    emsg += cmQtAutoGen::Quoted(key);
    emsg += "\n";
    emsg += "  RegExp: ";
    emsg += cmQtAutoGen::Quoted(regExp);
    emsg += "\n";
    this->LogError(cmQtAutoGen::MOC, emsg);
    return false;
  }
  return true;
}

void cmQtAutoGeneratorMocUic::MocFindDepends(std::string const& absFilename,
                                             std::string const& contentText,
                                             std::set<std::string>& depends)
{
  if (this->MocDependFilters.empty() && contentText.empty()) {
    return;
  }

  std::vector<std::string> matches;
  for (KeyRegExp& filter : this->MocDependFilters) {
    // Run a simple find string check
    if (contentText.find(filter.Key) != std::string::npos) {
      // Run the expensive regular expression check loop
      const char* contentChars = contentText.c_str();
      while (filter.RegExp.find(contentChars)) {
        std::string match = filter.RegExp.match(1);
        if (!match.empty()) {
          matches.emplace_back(std::move(match));
        }
        contentChars += filter.RegExp.end();
      }
    }
  }

  if (!matches.empty()) {
    std::string const sourcePath = SubDirPrefix(absFilename);
    for (std::string const& match : matches) {
      // Find the dependency file
      std::string incFile;
      if (this->MocFindIncludedFile(incFile, sourcePath, match)) {
        depends.insert(incFile);
        if (this->GetVerbose()) {
          this->LogInfo(cmQtAutoGen::MOC, "Found dependency:\n  " +
                          cmQtAutoGen::Quoted(absFilename) + "\n  " +
                          cmQtAutoGen::Quoted(incFile));
        }
      } else {
        this->LogFileWarning(cmQtAutoGen::MOC, absFilename,
                             "Could not find dependency file " +
                               cmQtAutoGen::Quoted(match));
      }
    }
  }
}

/**
 * @return True on success
 */
bool cmQtAutoGeneratorMocUic::MocParseSourceContent(
  std::string const& absFilename, std::string const& contentText)
{
  if (this->GetVerbose()) {
    this->LogInfo(cmQtAutoGen::MOC, "Checking: " + absFilename);
  }

  auto AddJob = [this, &absFilename](std::string const& sourceFile,
                                     std::string const& includeString,
                                     std::string const* content) {
    auto job = cm::make_unique<MocJobIncluded>();
    job->SourceFile = sourceFile;
    job->BuildFileRel = this->AutogenIncludeDir;
    job->BuildFileRel += includeString;
    job->Includer = absFilename;
    job->IncludeString = includeString;
    job->DependsValid = (content != nullptr);
    if (job->DependsValid) {
      this->MocFindDepends(sourceFile, *content, job->Depends);
    }
    this->MocJobsIncluded.push_back(std::move(job));
  };

  struct MocInc
  {
    std::string Inc;  // full include string
    std::string Dir;  // include string directory
    std::string Base; // include string file base
  };

  // Extract moc includes from file
  std::vector<MocInc> mocIncsUsc;
  std::vector<MocInc> mocIncsDot;
  {
    const char* contentChars = contentText.c_str();
    if (strstr(contentChars, "moc") != nullptr) {
      while (this->MocRegExpInclude.find(contentChars)) {
        std::string incString = this->MocRegExpInclude.match(1);
        std::string incDir(SubDirPrefix(incString));
        std::string incBase =
          cmSystemTools::GetFilenameWithoutLastExtension(incString);
        if (cmHasLiteralPrefix(incBase, "moc_")) {
          // moc_<BASE>.cxx
          // Remove the moc_ part from the base name
          mocIncsUsc.push_back(MocInc{ std::move(incString), std::move(incDir),
                                       incBase.substr(4) });
        } else {
          // <BASE>.moc
          mocIncsDot.push_back(MocInc{ std::move(incString), std::move(incDir),
                                       std::move(incBase) });
        }
        // Forward content pointer
        contentChars += this->MocRegExpInclude.end();
      }
    }
  }

  std::string selfMacroName;
  const bool selfRequiresMoc = this->MocRequired(contentText, &selfMacroName);

  // Check if there is anything to do
  if (!selfRequiresMoc && mocIncsUsc.empty() && mocIncsDot.empty()) {
    return true;
  }

  // Scan file variables
  std::string const scanFileDir = SubDirPrefix(absFilename);
  std::string const scanFileBase =
    cmSystemTools::GetFilenameWithoutLastExtension(absFilename);
  // Relaxed mode variables
  bool ownDotMocIncluded = false;
  std::string ownMocUscInclude;
  std::string ownMocUscHeader;

  // Process moc_<BASE>.cxx includes
  for (const MocInc& mocInc : mocIncsUsc) {
    std::string const header =
      this->MocFindIncludedHeader(scanFileDir, mocInc.Dir + mocInc.Base);
    if (!header.empty()) {
      // Check if header is skipped
      if (this->MocSkip(header)) {
        continue;
      }
      // Register moc job
      AddJob(header, mocInc.Inc, nullptr);
      // Store meta information for relaxed mode
      if (this->MocRelaxedMode && (mocInc.Base == scanFileBase)) {
        ownMocUscInclude = mocInc.Inc;
        ownMocUscHeader = header;
      }
    } else {
      std::string emsg = "The file includes the moc file ";
      emsg += cmQtAutoGen::Quoted(mocInc.Inc);
      emsg += ", but could not find the header ";
      emsg += cmQtAutoGen::Quoted(this->MocStringHeaders(mocInc.Base));
      this->LogFileError(cmQtAutoGen::MOC, absFilename, emsg);
      return false;
    }
  }

  // Process <BASE>.moc includes
  for (const MocInc& mocInc : mocIncsDot) {
    const bool ownMoc = (mocInc.Base == scanFileBase);
    if (this->MocRelaxedMode) {
      // Relaxed mode
      if (selfRequiresMoc && ownMoc) {
        // Add self
        AddJob(absFilename, mocInc.Inc, &contentText);
        ownDotMocIncluded = true;
      } else {
        // In relaxed mode try to find a header instead but issue a warning.
        // This is for KDE4 compatibility
        std::string const header =
          this->MocFindIncludedHeader(scanFileDir, mocInc.Dir + mocInc.Base);
        if (!header.empty()) {
          // Check if header is skipped
          if (this->MocSkip(header)) {
            continue;
          }
          // Register moc job
          AddJob(header, mocInc.Inc, nullptr);
          if (!selfRequiresMoc) {
            if (ownMoc) {
              std::string emsg = "The file includes the moc file ";
              emsg += cmQtAutoGen::Quoted(mocInc.Inc);
              emsg += ", but does not contain a ";
              emsg += this->MocStringMacros();
              emsg += " macro.\nRunning moc on\n  ";
              emsg += cmQtAutoGen::Quoted(header);
              emsg += "!\nBetter include ";
              emsg += cmQtAutoGen::Quoted("moc_" + mocInc.Base + ".cpp");
              emsg += " for a compatibility with strict mode.\n"
                      "(CMAKE_AUTOMOC_RELAXED_MODE warning)\n";
              this->LogFileWarning(cmQtAutoGen::MOC, absFilename, emsg);
            } else {
              std::string emsg = "The file includes the moc file ";
              emsg += cmQtAutoGen::Quoted(mocInc.Inc);
              emsg += " instead of ";
              emsg += cmQtAutoGen::Quoted("moc_" + mocInc.Base + ".cpp");
              emsg += ".\nRunning moc on\n  ";
              emsg += cmQtAutoGen::Quoted(header);
              emsg += "!\nBetter include ";
              emsg += cmQtAutoGen::Quoted("moc_" + mocInc.Base + ".cpp");
              emsg += " for compatibility with strict mode.\n"
                      "(CMAKE_AUTOMOC_RELAXED_MODE warning)\n";
              this->LogFileWarning(cmQtAutoGen::MOC, absFilename, emsg);
            }
          }
        } else {
          std::string emsg = "The file includes the moc file ";
          emsg += cmQtAutoGen::Quoted(mocInc.Inc);
          emsg += ", which seems to be the moc file from a different "
                  "source file. CMake also could not find a matching "
                  "header.";
          this->LogFileError(cmQtAutoGen::MOC, absFilename, emsg);
          return false;
        }
      }
    } else {
      // Strict mode
      if (ownMoc) {
        // Include self
        AddJob(absFilename, mocInc.Inc, &contentText);
        ownDotMocIncluded = true;
        // Accept but issue a warning if moc isn't required
        if (!selfRequiresMoc) {
          std::string emsg = "The file includes the moc file ";
          emsg += cmQtAutoGen::Quoted(mocInc.Inc);
          emsg += ", but does not contain a ";
          emsg += this->MocStringMacros();
          emsg += " macro.";
          this->LogFileWarning(cmQtAutoGen::MOC, absFilename, emsg);
        }
      } else {
        // Don't allow <BASE>.moc include other than self in strict mode
        std::string emsg = "The file includes the moc file ";
        emsg += cmQtAutoGen::Quoted(mocInc.Inc);
        emsg += ", which seems to be the moc file from a different "
                "source file.\nThis is not supported. Include ";
        emsg += cmQtAutoGen::Quoted(scanFileBase + ".moc");
        emsg += " to run moc on this source file.";
        this->LogFileError(cmQtAutoGen::MOC, absFilename, emsg);
        return false;
      }
    }
  }

  if (selfRequiresMoc && !ownDotMocIncluded) {
    // In this case, check whether the scanned file itself contains a Q_OBJECT.
    // If this is the case, the moc_foo.cpp should probably be generated from
    // foo.cpp instead of foo.h, because otherwise it won't build.
    // But warn, since this is not how it is supposed to be used.
    if (this->MocRelaxedMode && !ownMocUscInclude.empty()) {
      // This is for KDE4 compatibility:
      std::string emsg = "The file contains a ";
      emsg += selfMacroName;
      emsg += " macro, but does not include ";
      emsg += cmQtAutoGen::Quoted(scanFileBase + ".moc");
      emsg += ". Instead it includes ";
      emsg += cmQtAutoGen::Quoted(ownMocUscInclude);
      emsg += ".\nRunning moc on\n  ";
      emsg += cmQtAutoGen::Quoted(absFilename);
      emsg += "!\nBetter include ";
      emsg += cmQtAutoGen::Quoted(scanFileBase + ".moc");
      emsg += " for compatibility with strict mode.\n"
              "(CMAKE_AUTOMOC_RELAXED_MODE warning)";
      this->LogFileWarning(cmQtAutoGen::MOC, absFilename, emsg);

      // Remove own header job
      {
        auto itC = this->MocJobsIncluded.begin();
        auto itE = this->MocJobsIncluded.end();
        for (; itC != itE; ++itC) {
          if ((*itC)->SourceFile == ownMocUscHeader) {
            if ((*itC)->IncludeString == ownMocUscInclude) {
              this->MocJobsIncluded.erase(itC);
              break;
            }
          }
        }
      }
      // Add own source job
      AddJob(absFilename, ownMocUscInclude, &contentText);
    } else {
      // Otherwise always error out since it will not compile:
      std::string emsg = "The file contains a ";
      emsg += selfMacroName;
      emsg += " macro, but does not include ";
      emsg += cmQtAutoGen::Quoted(scanFileBase + ".moc");
      emsg += "!\nConsider to\n - add #include \"";
      emsg += scanFileBase;
      emsg += ".moc\"\n - enable SKIP_AUTOMOC for this file";
      this->LogFileError(cmQtAutoGen::MOC, absFilename, emsg);
      return false;
    }
  }
  return true;
}

void cmQtAutoGeneratorMocUic::MocParseHeaderContent(
  std::string const& absFilename, std::string const& contentText)
{
  if (this->GetVerbose()) {
    this->LogInfo(cmQtAutoGen::MOC, "Checking: " + absFilename);
  }

  auto const fit =
    std::find_if(this->MocJobsIncluded.cbegin(), this->MocJobsIncluded.cend(),
                 [&absFilename](std::unique_ptr<MocJobIncluded> const& job) {
                   return job->SourceFile == absFilename;
                 });
  if (fit == this->MocJobsIncluded.cend()) {
    if (this->MocRequired(contentText)) {
      auto job = cm::make_unique<MocJobAuto>();
      job->SourceFile = absFilename;
      {
        std::string& bld = job->BuildFileRel;
        bld = this->FilePathChecksum.getPart(absFilename);
        bld += '/';
        bld += "moc_";
        bld += cmSystemTools::GetFilenameWithoutLastExtension(absFilename);
        if (this->MultiConfig != cmQtAutoGen::SINGLE) {
          bld += this->ConfigSuffix;
        }
        bld += ".cpp";
      }
      this->MocFindDepends(absFilename, contentText, job->Depends);
      this->MocJobsAuto.push_back(std::move(job));
    }
  }
}

bool cmQtAutoGeneratorMocUic::MocGenerateAll()
{
  if (!this->MocEnabled()) {
    return true;
  }

  // Look for name collisions in included moc files
  {
    bool collision = false;
    std::map<std::string, std::vector<MocJobIncluded const*>> collisions;
    for (auto const& job : this->MocJobsIncluded) {
      auto& list = collisions[job->IncludeString];
      if (!list.empty()) {
        collision = true;
      }
      list.push_back(job.get());
    }
    if (collision) {
      std::string emsg =
        "Included moc files with the same name will be "
        "generated from different sources.\n"
        "Consider to\n"
        " - not include the \"moc_<NAME>.cpp\" file\n"
        " - add a directory prefix to a \"<NAME>.moc\" include "
        "(e.g \"sub/<NAME>.moc\")\n"
        " - rename the source file(s)\n"
        "Include conflicts\n"
        "-----------------\n";
      const auto& colls = collisions;
      for (auto const& coll : colls) {
        if (coll.second.size() > 1) {
          emsg += cmQtAutoGen::Quoted(coll.first);
          emsg += " included in\n";
          for (const MocJobIncluded* job : coll.second) {
            emsg += " - ";
            emsg += cmQtAutoGen::Quoted(job->Includer);
            emsg += "\n";
          }
          emsg += "would be generated from\n";
          for (const MocJobIncluded* job : coll.second) {
            emsg += " - ";
            emsg += cmQtAutoGen::Quoted(job->SourceFile);
            emsg += "\n";
          }
        }
      }
      this->LogError(cmQtAutoGen::MOC, emsg);
      return false;
    }
  }

  // (Re)generate moc_predefs.h on demand
  if (!this->MocPredefsCmd.empty()) {
    if (this->MocSettingsChanged ||
        !cmSystemTools::FileExists(this->MocPredefsFileAbs)) {
      if (this->GetVerbose()) {
        this->LogBold("Generating MOC predefs " + this->MocPredefsFileRel);
      }

      std::string output;
      {
        // Compose command
        std::vector<std::string> cmd = this->MocPredefsCmd;
        // Add includes
        cmd.insert(cmd.end(), this->MocIncludes.begin(),
                   this->MocIncludes.end());
        // Add definitions
        for (std::string const& def : this->MocDefinitions) {
          cmd.push_back("-D" + def);
        }
        // Execute command
        if (!this->RunCommand(cmd, output)) {
          this->LogCommandError(cmQtAutoGen::MOC,
                                "moc_predefs generation failed", cmd, output);
          return false;
        }
      }

      // (Re)write predefs file only on demand
      if (this->FileDiffers(this->MocPredefsFileAbs, output)) {
        if (this->FileWrite(cmQtAutoGen::MOC, this->MocPredefsFileAbs,
                            output)) {
          this->MocPredefsChanged = true;
        } else {
          this->LogFileError(cmQtAutoGen::MOC, this->MocPredefsFileAbs,
                             "moc_predefs file writing failed");
          return false;
        }
      } else {
        // Touch to update the time stamp
        if (this->GetVerbose()) {
          this->LogInfo(cmQtAutoGen::MOC,
                        "Touching moc_predefs " + this->MocPredefsFileRel);
        }
        cmSystemTools::Touch(this->MocPredefsFileAbs, false);
      }
    }

    // Add moc_predefs.h to moc file dependencies
    for (auto const& item : this->MocJobsIncluded) {
      item->Depends.insert(this->MocPredefsFileAbs);
    }
    for (auto const& item : this->MocJobsAuto) {
      item->Depends.insert(this->MocPredefsFileAbs);
    }
  }

  // Generate moc files that are included by source files.
  for (auto const& item : this->MocJobsIncluded) {
    if (!this->MocGenerateFile(*item)) {
      return false;
    }
  }
  // Generate moc files that are _not_ included by source files.
  bool autoNameGenerated = false;
  for (auto const& item : this->MocJobsAuto) {
    if (!this->MocGenerateFile(*item, &autoNameGenerated)) {
      return false;
    }
  }

  // Compose mocs compilation file content
  {
    std::string mocs =
      "// This file is autogenerated. Changes will be overwritten.\n";
    if (this->MocJobsAuto.empty()) {
      // Placeholder content
      mocs +=
        "// No files found that require moc or the moc files are included\n";
      mocs += "enum some_compilers { need_more_than_nothing };\n";
    } else {
      // Valid content
      for (const auto& item : this->MocJobsAuto) {
        mocs += "#include \"";
        mocs += item->BuildFileRel;
        mocs += "\"\n";
      }
    }

    if (this->FileDiffers(this->MocCompFileAbs, mocs)) {
      // Actually write mocs compilation file
      if (this->GetVerbose()) {
        this->LogBold("Generating MOC compilation " + this->MocCompFileRel);
      }
      if (!this->FileWrite(cmQtAutoGen::MOC, this->MocCompFileAbs, mocs)) {
        this->LogFileError(cmQtAutoGen::MOC, this->MocCompFileAbs,
                           "mocs compilation file writing failed");
        return false;
      }
    } else if (autoNameGenerated) {
      // Only touch mocs compilation file
      if (this->GetVerbose()) {
        this->LogInfo(cmQtAutoGen::MOC,
                      "Touching mocs compilation " + this->MocCompFileRel);
      }
      cmSystemTools::Touch(this->MocCompFileAbs, false);
    }
  }

  return true;
}

/**
 * @return True on success
 */
bool cmQtAutoGeneratorMocUic::MocGenerateFile(const MocJobAuto& mocJob,
                                              bool* generated)
{
  bool success = true;

  std::string const mocFileAbs = cmSystemTools::CollapseCombinedPath(
    this->AutogenBuildDir, mocJob.BuildFileRel);

  bool generate = false;
  std::string generateReason;
  if (!generate && !cmSystemTools::FileExists(mocFileAbs.c_str())) {
    if (this->GetVerbose()) {
      generateReason = "Generating ";
      generateReason += cmQtAutoGen::Quoted(mocFileAbs);
      generateReason += " from its source file ";
      generateReason += cmQtAutoGen::Quoted(mocJob.SourceFile);
      generateReason += " because it doesn't exist";
    }
    generate = true;
  }
  if (!generate && this->MocSettingsChanged) {
    if (this->GetVerbose()) {
      generateReason = "Generating ";
      generateReason += cmQtAutoGen::Quoted(mocFileAbs);
      generateReason += " from ";
      generateReason += cmQtAutoGen::Quoted(mocJob.SourceFile);
      generateReason += " because the MOC settings changed";
    }
    generate = true;
  }
  if (!generate && this->MocPredefsChanged) {
    if (this->GetVerbose()) {
      generateReason = "Generating ";
      generateReason += cmQtAutoGen::Quoted(mocFileAbs);
      generateReason += " from ";
      generateReason += cmQtAutoGen::Quoted(mocJob.SourceFile);
      generateReason += " because moc_predefs.h changed";
    }
    generate = true;
  }
  if (!generate) {
    std::string error;
    if (FileIsOlderThan(mocFileAbs, mocJob.SourceFile, &error)) {
      if (this->GetVerbose()) {
        generateReason = "Generating ";
        generateReason += cmQtAutoGen::Quoted(mocFileAbs);
        generateReason += " because it's older than its source file ";
        generateReason += cmQtAutoGen::Quoted(mocJob.SourceFile);
      }
      generate = true;
    } else {
      if (!error.empty()) {
        this->LogError(cmQtAutoGen::MOC, error);
        success = false;
      }
    }
  }
  if (success && !generate) {
    // Test if a dependency file is newer
    std::string error;
    for (std::string const& depFile : mocJob.Depends) {
      if (FileIsOlderThan(mocFileAbs, depFile, &error)) {
        if (this->GetVerbose()) {
          generateReason = "Generating ";
          generateReason += cmQtAutoGen::Quoted(mocFileAbs);
          generateReason += " from ";
          generateReason += cmQtAutoGen::Quoted(mocJob.SourceFile);
          generateReason += " because it is older than ";
          generateReason += cmQtAutoGen::Quoted(depFile);
        }
        generate = true;
        break;
      }
      if (!error.empty()) {
        this->LogError(cmQtAutoGen::MOC, error);
        success = false;
        break;
      }
    }
  }

  if (generate) {
    // Log
    if (this->GetVerbose()) {
      this->LogBold("Generating MOC source " + mocJob.BuildFileRel);
      this->LogInfo(cmQtAutoGen::MOC, generateReason);
    }

    // Make sure the parent directory exists
    if (this->MakeParentDirectory(cmQtAutoGen::MOC, mocFileAbs)) {
      // Compose moc command
      std::vector<std::string> cmd;
      cmd.push_back(this->MocExecutable);
      // Add options
      cmd.insert(cmd.end(), this->MocAllOptions.begin(),
                 this->MocAllOptions.end());
      // Add predefs include
      if (!this->MocPredefsFileAbs.empty()) {
        cmd.push_back("--include");
        cmd.push_back(this->MocPredefsFileAbs);
      }
      cmd.push_back("-o");
      cmd.push_back(mocFileAbs);
      cmd.push_back(mocJob.SourceFile);

      // Execute moc command
      std::string output;
      if (this->RunCommand(cmd, output)) {
        // Success
        if (generated != nullptr) {
          *generated = true;
        }
      } else {
        // Moc command failed
        {
          std::string emsg = "moc failed for\n  ";
          emsg += cmQtAutoGen::Quoted(mocJob.SourceFile);
          this->LogCommandError(cmQtAutoGen::MOC, emsg, cmd, output);
        }
        cmSystemTools::RemoveFile(mocFileAbs);
        success = false;
      }
    } else {
      // Parent directory creation failed
      success = false;
    }
  }
  return success;
}

/**
 * @brief Tests if the file name is in the skip list
 */
bool cmQtAutoGeneratorMocUic::UicSkip(std::string const& absFilename) const
{
  if (this->UicEnabled()) {
    // Test if the file name is on the skip list
    if (!ListContains(this->UicSkipList, absFilename)) {
      return false;
    }
  }
  return true;
}

bool cmQtAutoGeneratorMocUic::UicParseContent(std::string const& absFilename,
                                              std::string const& contentText)
{
  if (this->GetVerbose()) {
    this->LogInfo(cmQtAutoGen::UIC, "Checking: " + absFilename);
  }

  std::vector<std::string> includes;
  // Extracte includes
  {
    const char* contentChars = contentText.c_str();
    if (strstr(contentChars, "ui_") != nullptr) {
      while (this->UicRegExpInclude.find(contentChars)) {
        includes.push_back(this->UicRegExpInclude.match(1));
        contentChars += this->UicRegExpInclude.end();
      }
    }
  }

  for (std::string const& includeString : includes) {
    std::string uiInputFile;
    if (!UicFindIncludedFile(uiInputFile, absFilename, includeString)) {
      return false;
    }
    // Check if this file should be skipped
    if (this->UicSkip(uiInputFile)) {
      continue;
    }
    // Check if the job already exists
    bool jobExists = false;
    for (const auto& job : this->UicJobs) {
      if ((job->SourceFile == uiInputFile) &&
          (job->IncludeString == includeString)) {
        jobExists = true;
        break;
      }
    }
    if (!jobExists) {
      auto job = cm::make_unique<UicJob>();
      job->SourceFile = uiInputFile;
      job->BuildFileRel = this->AutogenIncludeDir;
      job->BuildFileRel += includeString;
      job->Includer = absFilename;
      job->IncludeString = includeString;
      this->UicJobs.push_back(std::move(job));
    }
  }

  return true;
}

bool cmQtAutoGeneratorMocUic::UicFindIncludedFile(
  std::string& absFile, std::string const& sourceFile,
  std::string const& includeString)
{
  bool success = false;
  std::string searchFile =
    cmSystemTools::GetFilenameWithoutLastExtension(includeString).substr(3);
  searchFile += ".ui";
  // Collect search paths list
  std::vector<std::string> testFiles;
  {
    std::string const searchPath = SubDirPrefix(includeString);

    std::string searchFileFull;
    if (!searchPath.empty()) {
      searchFileFull = searchPath;
      searchFileFull += searchFile;
    }
    // Vicinity of the source
    {
      std::string const sourcePath = SubDirPrefix(sourceFile);
      testFiles.push_back(sourcePath + searchFile);
      if (!searchPath.empty()) {
        testFiles.push_back(sourcePath + searchFileFull);
      }
    }
    // AUTOUIC search paths
    if (!this->UicSearchPaths.empty()) {
      for (std::string const& sPath : this->UicSearchPaths) {
        testFiles.push_back((sPath + "/").append(searchFile));
      }
      if (!searchPath.empty()) {
        for (std::string const& sPath : this->UicSearchPaths) {
          testFiles.push_back((sPath + "/").append(searchFileFull));
        }
      }
    }
  }

  // Search for the .ui file!
  for (std::string const& testFile : testFiles) {
    if (cmSystemTools::FileExists(testFile.c_str())) {
      absFile = cmSystemTools::GetRealPath(testFile);
      success = true;
      break;
    }
  }

  // Log error
  if (!success) {
    std::string emsg = "Could not find ";
    emsg += cmQtAutoGen::Quoted(searchFile);
    emsg += " in\n";
    for (std::string const& testFile : testFiles) {
      emsg += "  ";
      emsg += cmQtAutoGen::Quoted(testFile);
      emsg += "\n";
    }
    this->LogFileError(cmQtAutoGen::UIC, sourceFile, emsg);
  }

  return success;
}

bool cmQtAutoGeneratorMocUic::UicGenerateAll()
{
  if (!this->UicEnabled()) {
    return true;
  }

  // Look for name collisions in included uic files
  {
    bool collision = false;
    std::map<std::string, std::vector<UicJob const*>> collisions;
    for (auto const& job : this->UicJobs) {
      auto& list = collisions[job->IncludeString];
      if (!list.empty()) {
        collision = true;
      }
      list.push_back(job.get());
    }
    if (collision) {
      std::string emsg =
        "Included uic files with the same name will be "
        "generated from different sources.\n"
        "Consider to\n"
        " - add a directory prefix to a \"ui_<NAME>.h\" include "
        "(e.g \"sub/ui_<NAME>.h\")\n"
        " - rename the <NAME>.ui file(s) and adjust the \"ui_<NAME>.h\" "
        "include(s)\n"
        "Include conflicts\n"
        "-----------------\n";
      const auto& colls = collisions;
      for (auto const& coll : colls) {
        if (coll.second.size() > 1) {
          emsg += cmQtAutoGen::Quoted(coll.first);
          emsg += " included in\n";
          for (const UicJob* job : coll.second) {
            emsg += " - ";
            emsg += cmQtAutoGen::Quoted(job->Includer);
            emsg += "\n";
          }
          emsg += "would be generated from\n";
          for (const UicJob* job : coll.second) {
            emsg += " - ";
            emsg += cmQtAutoGen::Quoted(job->SourceFile);
            emsg += "\n";
          }
        }
      }
      this->LogError(cmQtAutoGen::UIC, emsg);
      return false;
    }
  }

  // Generate ui header files
  for (const auto& item : this->UicJobs) {
    if (!this->UicGenerateFile(*item)) {
      return false;
    }
  }

  return true;
}

/**
 * @return True on success
 */
bool cmQtAutoGeneratorMocUic::UicGenerateFile(const UicJob& uicJob)
{
  bool success = true;

  std::string const uicFileAbs = cmSystemTools::CollapseCombinedPath(
    this->AutogenBuildDir, uicJob.BuildFileRel);

  bool generate = false;
  std::string generateReason;
  if (!generate && !cmSystemTools::FileExists(uicFileAbs.c_str())) {
    if (this->GetVerbose()) {
      generateReason = "Generating ";
      generateReason += cmQtAutoGen::Quoted(uicFileAbs);
      generateReason += " from its source file ";
      generateReason += cmQtAutoGen::Quoted(uicJob.SourceFile);
      generateReason += " because it doesn't exist";
    }
    generate = true;
  }
  if (!generate && this->UicSettingsChanged) {
    if (this->GetVerbose()) {
      generateReason = "Generating ";
      generateReason += cmQtAutoGen::Quoted(uicFileAbs);
      generateReason += " from ";
      generateReason += cmQtAutoGen::Quoted(uicJob.SourceFile);
      generateReason += " because the UIC settings changed";
    }
    generate = true;
  }
  if (!generate) {
    std::string error;
    if (FileIsOlderThan(uicFileAbs, uicJob.SourceFile, &error)) {
      if (this->GetVerbose()) {
        generateReason = "Generating ";
        generateReason += cmQtAutoGen::Quoted(uicFileAbs);
        generateReason += " because it's older than its source file ";
        generateReason += cmQtAutoGen::Quoted(uicJob.SourceFile);
      }
      generate = true;
    } else {
      if (!error.empty()) {
        this->LogError(cmQtAutoGen::UIC, error);
        success = false;
      }
    }
  }
  if (generate) {
    // Log
    if (this->GetVerbose()) {
      this->LogBold("Generating UIC header " + uicJob.BuildFileRel);
      this->LogInfo(cmQtAutoGen::UIC, generateReason);
    }

    // Make sure the parent directory exists
    if (this->MakeParentDirectory(cmQtAutoGen::UIC, uicFileAbs)) {
      // Compose uic command
      std::vector<std::string> cmd;
      cmd.push_back(this->UicExecutable);
      {
        std::vector<std::string> allOpts = this->UicTargetOptions;
        auto optionIt = this->UicOptions.find(uicJob.SourceFile);
        if (optionIt != this->UicOptions.end()) {
          cmQtAutoGen::UicMergeOptions(allOpts, optionIt->second,
                                       (this->QtVersionMajor == 5));
        }
        cmd.insert(cmd.end(), allOpts.begin(), allOpts.end());
      }
      cmd.push_back("-o");
      cmd.push_back(uicFileAbs);
      cmd.push_back(uicJob.SourceFile);

      std::string output;
      if (this->RunCommand(cmd, output)) {
        // Success
      } else {
        // Command failed
        {
          std::string emsg = "uic failed for\n  ";
          emsg += cmQtAutoGen::Quoted(uicJob.SourceFile);
          emsg += "\nincluded by\n  ";
          emsg += cmQtAutoGen::Quoted(uicJob.Includer);
          this->LogCommandError(cmQtAutoGen::UIC, emsg, cmd, output);
        }
        cmSystemTools::RemoveFile(uicFileAbs);
        success = false;
      }
    } else {
      // Parent directory creation failed
      success = false;
    }
  }
  return success;
}

/**
 * @brief Tries to find the header file to the given file base path by
 * appending different header extensions
 * @return True on success
 */
bool cmQtAutoGeneratorMocUic::FindHeader(std::string& header,
                                         std::string const& testBasePath) const
{
  for (std::string const& ext : this->HeaderExtensions) {
    std::string testFilePath(testBasePath);
    testFilePath.push_back('.');
    testFilePath += ext;
    if (cmSystemTools::FileExists(testFilePath.c_str())) {
      header = testFilePath;
      return true;
    }
  }
  return false;
}
