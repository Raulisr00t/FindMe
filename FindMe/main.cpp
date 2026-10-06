#include <Windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class DirectoryQueue
{
private:
    std::deque<std::wstring> queue;

    std::mutex mutex;
    std::condition_variable cv;

    uint64_t pending = 0;

    bool shutdown = false;

public:

    void Push(std::wstring&& directory)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);

            queue.emplace_back(std::move(directory));
            ++pending;
        }

        cv.notify_one();
    }

    bool Pop(std::wstring& directory)
    {
        std::unique_lock<std::mutex> lock(mutex);

        cv.wait(lock, [&]
            {
                return shutdown || !queue.empty();
            });

        if (shutdown && queue.empty())
            return false;

        directory = std::move(queue.front());
        queue.pop_front();

        return true;
    }

    void Complete()
    {
        std::lock_guard<std::mutex> lock(mutex);

        --pending;

        if (pending == 0)
        {   
            shutdown = true;
            cv.notify_all();
        }
    }

    void WaitForCompletion()
    {
        std::unique_lock<std::mutex> lock(mutex);

        cv.wait(lock, [&]
            {
                return pending == 0;
            });
    }
};


class FileSearcher
{
private:
    const wchar_t* target;

    DirectoryQueue directories;

    std::atomic<uint64_t> matches{ 0 };
    std::atomic<uint64_t> directoriesScanned{ 0 };
    std::atomic<uint64_t> filesScanned{ 0 };

    std::mutex outputMutex;

private:

    static bool IsDotDirectory(const wchar_t* name)
    {
        if (name[0] != L'.')
            return false;

        if (name[1] == L'\0')
            return true;

        return
            name[1] == L'.' &&
            name[2] == L'\0';
    }


    void PrintMatch(
        const std::wstring& directory,
        const wchar_t* filename)
    {
        std::lock_guard<std::mutex> lock(outputMutex);

        wprintf(
            L"[+] Found: %ls\\%ls\n",
            directory.c_str(),
            filename
        );
    }

    void ScanDirectory(const std::wstring& directory)
    {
        ++directoriesScanned;

        std::wstring searchPath;

        searchPath.reserve(
            directory.size() + 2
        );

        searchPath = directory;
        searchPath += L"\\*";

        WIN32_FIND_DATAW fd{};

        HANDLE hFind = FindFirstFileExW(
            searchPath.c_str(),

            FindExInfoBasic,

            &fd,

            FindExSearchNameMatch,

            nullptr,

            FIND_FIRST_EX_LARGE_FETCH
        );

        if (hFind == INVALID_HANDLE_VALUE)
        {
            directories.Complete();
            return;
        }

        do
        {
            const wchar_t* name = fd.cFileName;
            const DWORD attributes = fd.dwFileAttributes;

            if (IsDotDirectory(name))
                continue;

            if (attributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                if (attributes & FILE_ATTRIBUTE_REPARSE_POINT)
                    continue;

                const size_t nameLength = wcslen(name);

                std::wstring child;

                child.reserve(
                    directory.size() +
                    1 +
                    nameLength
                );

                child = directory;
                child += L'\\';
                child.append(name, nameLength);

                directories.Push(std::move(child));

                continue;
            }

            ++filesScanned;

            if (_wcsicmp(name, target) == 0)
            {
                ++matches;

                PrintMatch(
                    directory,
                    name
                );
            }

        } while (FindNextFileW(hFind, &fd));

        FindClose(hFind);

        directories.Complete();
    }


    void Worker()
    {
        std::wstring directory;

        while (directories.Pop(directory))
        {
            ScanDirectory(directory);
        }
    }


public:

    explicit FileSearcher(const wchar_t* filename)
        : target(filename)
    {
    }

    uint64_t Search(
        const std::wstring& root,
        unsigned int workerCount)
    {
        if (workerCount == 0)
            workerCount = 1;

        directories.Push(
            std::wstring(root)
        );

        std::vector<std::thread> workers;
        
        workers.reserve(workerCount);
         
        for (unsigned int i = 0; i < workerCount; ++i)
        {
            workers.emplace_back(
                &FileSearcher::Worker,
                this
            );
        }
        directories.WaitForCompletion();
        
        for (auto& worker : workers)
        {
            if (worker.joinable())
                worker.join();
        }

        return matches.load(
            std::memory_order_relaxed
        );
    }


    uint64_t GetMatches() const
    {
        return matches.load(
            std::memory_order_relaxed
        );
    }


    uint64_t GetDirectoriesScanned() const
    {
        return directoriesScanned.load(
            std::memory_order_relaxed
        );
    }


    uint64_t GetFilesScanned() const
    {
        return filesScanned.load(
            std::memory_order_relaxed
        );
    }
};


int wmain(int argc, wchar_t* argv[])
{
    if (argc != 3)
    {
        wprintf(
            L"Usage: %ls <Directory> <FileName>\n\n"
            L"Example:\n"
            L"  %ls C:\\ whoami.exe\n",
            argv[0],
            argv[0]
        );

        return 1;
    }

    const wchar_t* root = argv[1];
    const wchar_t* filename = argv[2];

    unsigned int workers =
        std::thread::hardware_concurrency();

    if (workers == 0)
        workers = 4;

    if (workers > 16)
        workers = 16;

    wprintf(
        L"[*] Searching : %ls\n"
        L"[*] Filename  : %ls\n"
        L"[*] Workers   : %u\n\n",
        root,
        filename,
        workers
    );

    FileSearcher searcher(filename);

    const uint64_t matchCount =
        searcher.Search(
            root,
            workers
        );

    wprintf(
        L"\n"
        L"[*] Search complete.\n"
        L"[*] Directories scanned : %llu\n"
        L"[*] Files scanned       : %llu\n"
        L"[*] Matches              : %llu\n",
        searcher.GetDirectoriesScanned(),
        searcher.GetFilesScanned(),
        matchCount
    );

    return matchCount > 0 ? 0 : 1;
}