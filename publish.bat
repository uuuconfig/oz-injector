@echo off
REM One-shot publish: create the GitHub repo (if needed) and push.
REM
REM Why this exists: the GitHub connector available in this workspace is a GitHub
REM App without the Administration permission, so POST /user/repos returns
REM   403 Resource not accessible by integration
REM and the repository cannot be created for you. Creating the repo is the only
REM step that needs a human; everything after it is scripted here.
REM
REM Requirements: git on PATH, and a GitHub token that can write to the repo
REM (classic PAT with repo scope, or a fine-grained PAT with Contents: read+write).
REM
REM Usage:
REM   publish.bat                 interactive, asks for the token
REM   publish.bat <token>          non-interactive

setlocal enabledelayedexpansion

set REPO_OWNER=uuuconfig
set REPO_NAME=oz-injector
set BRANCH=main

set TOKEN=%~1
if "%TOKEN%"=="" (
  set /p TOKEN=Paste a GitHub token with write access: 
)

if "%TOKEN%"=="" (
  echo [publish] no token given, aborting
  exit /b 1
)

echo [publish] repo   : https://github.com/%REPO_OWNER%/%REPO_NAME%
echo [publish] branch : %BRANCH%
echo.

REM --- 1. create the repo -----------------------------------------------------
REM Only works if the token belongs to %REPO_OWNER% itself. If the repo already
REM exists this returns 422 and we carry on.
echo [publish] ensuring the repository exists
curl -s -o NUL -w "  create attempt: HTTP %%{http_code}\n" ^
  -X POST ^
  -H "Authorization: Bearer %TOKEN%" ^
  -H "Accept: application/vnd.github+json" ^
  -H "X-GitHub-Api-Version: 2022-11-28" ^
  -d "{\"name\":\"%REPO_NAME%\",\"description\":\"Standalone x64 DLL injector with manual-map and LoadLibrary modes. Win32 + GDI, no Qt, no runtime deps.\",\"private\":false,\"auto_init\":false}" ^
  https://api.github.com/user/repos

echo [publish] confirming the repo is reachable
curl -s -o NUL -w "  repo check: HTTP %%{http_code}\n" ^
  -H "Authorization: Bearer %TOKEN%" ^
  -H "Accept: application/vnd.github+json" ^
  https://api.github.com/repos/%REPO_OWNER%/%REPO_NAME%

REM --- 2. push ---------------------------------------------------------------
REM The remote is driven through an explicit URL so the token is never written
REM into .git/config on disk.
set REMOTE_URL=https://x-access-token:%TOKEN%@github.com/%REPO_OWNER%/%REPO_NAME%.git

echo [publish] pushing %BRANCH%
git push -u "%REMOTE_URL%" %BRANCH% || (
  echo [publish] push failed
  exit /b 1
)

echo.
echo [publish] done -^> https://github.com/%REPO_OWNER%/%REPO_NAME%
endlocal
exit /b 0
