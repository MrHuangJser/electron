// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE-CHROMIUM file.

#include "shell/browser/ui/devtools_ui.h"

#include <memory>
#include <string>
#include <utility>

#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/memory/ref_counted_memory.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/task/thread_pool.h"
#include "chrome/common/webui_url_constants.h"
#include "content/public/browser/devtools_frontend_host.h"
#include "content/public/browser/url_data_source.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "net/base/filename_util.h"

// static
GURL GetCustomDevToolsFrontendURL() {
  base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();
  if (command_line->HasSwitch(switches::kCustomDevtoolsFrontend)) {
    return GURL(base::CommandLine::ForCurrentProcess()->GetSwitchValueASCII(
        "custom-devtools-frontend"));
  }
  return GURL();
}

namespace electron {

namespace {

std::string PathWithoutParams(const std::string& path) {
  return GURL(base::StrCat({content::kChromeDevToolsScheme,
                            url::kStandardSchemeSeparator,
                            chrome::kChromeUIDevToolsHost}))
      .Resolve(path)
      .path()
      .substr(1);
}

std::string GetMimeTypeForUrl(const GURL& url) {
  std::string filename = url.ExtractFileName();
  if (base::EndsWith(filename, ".html", base::CompareCase::INSENSITIVE_ASCII)) {
    return "text/html";
  } else if (base::EndsWith(filename, ".css",
                            base::CompareCase::INSENSITIVE_ASCII)) {
    return "text/css";
  } else if (base::EndsWith(filename, ".js",
                            base::CompareCase::INSENSITIVE_ASCII) ||
             base::EndsWith(filename, ".mjs",
                            base::CompareCase::INSENSITIVE_ASCII)) {
    return "application/javascript";
  } else if (base::EndsWith(filename, ".png",
                            base::CompareCase::INSENSITIVE_ASCII)) {
    return "image/png";
  } else if (base::EndsWith(filename, ".map",
                            base::CompareCase::INSENSITIVE_ASCII)) {
    return "application/json";
  } else if (base::EndsWith(filename, ".ts",
                            base::CompareCase::INSENSITIVE_ASCII)) {
    return "application/x-typescript";
  } else if (base::EndsWith(filename, ".gif",
                            base::CompareCase::INSENSITIVE_ASCII)) {
    return "image/gif";
  } else if (base::EndsWith(filename, ".svg",
                            base::CompareCase::INSENSITIVE_ASCII)) {
    return "image/svg+xml";
  } else if (base::EndsWith(filename, ".manifest",
                            base::CompareCase::INSENSITIVE_ASCII)) {
    return "text/cache-manifest";
  }
  return "text/html";
}

class BundledDataSource : public content::URLDataSource {
 public:
  BundledDataSource() = default;
  ~BundledDataSource() override = default;

  // disable copy
  BundledDataSource(const BundledDataSource&) = delete;
  BundledDataSource& operator=(const BundledDataSource&) = delete;

  // content::URLDataSource implementation.
  std::string GetSource() override { return chrome::kChromeUIDevToolsHost; }

  void StartDataRequest(const GURL& url,
                        const content::WebContents::Getter& wc_getter,
                        GotDataCallback callback) override {
    const std::string path = content::URLDataSource::URLToRequestPath(url);
    // Serve request from local bundle.
    std::string bundled_path_prefix(chrome::kChromeUIDevToolsBundledPath);
    bundled_path_prefix += "/";
    GURL custom_devtools_frontend = GetCustomDevToolsFrontendURL();
    if (!custom_devtools_frontend.is_valid()) {
      if (base::StartsWith(path, bundled_path_prefix,
                           base::CompareCase::INSENSITIVE_ASCII)) {
        StartBundledDataRequest(path.substr(bundled_path_prefix.length()),
                                std::move(callback));
        return;
      }
      return;
    }

    if (GetCustomDevToolsFrontendURL().SchemeIsFile()) {
      // Fetch from file system.
      StartFileRequest(path_under_bundled, std::move(callback));
      return;
    }

    // We do not handle remote and custom requests.
    std::move(callback).Run(nullptr);
  }

  std::string GetMimeType(const GURL& url) override {
    return GetMimeTypeForUrl(url);
  }

  bool ShouldAddContentSecurityPolicy() override { return false; }

  bool ShouldDenyXFrameOptions() override { return false; }

  bool ShouldServeMimeTypeAsContentTypeHeader() override { return true; }

  void StartBundledDataRequest(const std::string& path,
                               GotDataCallback callback) {
    std::string filename = PathWithoutParams(path);
    scoped_refptr<base::RefCountedMemory> bytes =
        content::DevToolsFrontendHost::GetFrontendResourceBytes(filename);

    DLOG_IF(WARNING, !bytes)
        << "Unable to find dev tool resource: " << filename
        << ". If you compiled with debug_devtools=1, try running with "
           "--debug-devtools.";
    std::move(callback).Run(bytes);
  }

  scoped_refptr<base::RefCountedMemory> ReadFileForDevTools(
      const base::FilePath& path) {
    std::string buffer;
    // if (!base::ReadFileToString(path, &buffer)) {
    //   LOG(ERROR) << "Failed to read " << path;
    //   return CreateNotFoundResponse();
    // }
    return base::RefCountedString::TakeString(&buffer);
  }

  void StartFileRequest(const std::string& path, GotDataCallback callback) {
    base::FilePath base_path;
    GURL custom_devtools_frontend = GetCustomDevToolsFrontendURL();
    DCHECK(custom_devtools_frontend.SchemeIsFile());
    // if (!net::FileURLToFilePath(custom_devtools_frontend, &base_path)) {
    //   std::move(callback).Run(CreateNotFoundResponse());
    //   LOG(WARNING) << "Unable to find DevTools resource: " << path;
    //   return;
    // }
    base::FilePath full_path = base_path.AppendASCII(path);
    CHECK(base_path.IsParent(full_path));

    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE,
        {base::MayBlock(), base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN,
         base::TaskPriority::USER_VISIBLE},
        base::BindOnce(ReadFileForDevTools, std::move(full_path)),
        std::move(callback));
  }
};

}  // namespace

DevToolsUI::DevToolsUI(content::BrowserContext* browser_context,
                       content::WebUI* web_ui)
    : WebUIController(web_ui) {
  web_ui->SetBindings(0);
  content::URLDataSource::Add(browser_context,
                              std::make_unique<BundledDataSource>());
}

}  // namespace electron
