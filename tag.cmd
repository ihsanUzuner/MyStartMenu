@echo off
rem Tags the current commit with the version in VERSION, as v<major.minor.patch>.
rem Bump VERSION and commit it first; pushing the tag is left to you (git push origin v...).
setlocal
cd /d "%~dp0"
set /p VERSION=<VERSION
git rev-parse -q --verify "refs/tags/v%VERSION%" >nul && (
    echo v%VERSION% already exists; bump VERSION first.
    exit /b 1
)
git diff --quiet HEAD -- VERSION || (
    echo VERSION has uncommitted changes; commit it first.
    exit /b 1
)
git tag -a "v%VERSION%" -m "MyStartMenu %VERSION%" && echo Tagged v%VERSION%.
