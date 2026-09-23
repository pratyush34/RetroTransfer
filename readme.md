

Pratyush
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Cmake build command:
$env:Path = "C:\msys64\ucrt64\bin;C:\Program Files\CMake\bin;$env:Path"
cmake --build build-ucrt64
.\build-ucrt64\RetroTransfer.exe . //. represents current directory


Cmake run command (new terimal):
$env:Path = "C:\msys64\ucrt64\bin;C:\Program Files\CMake\bin;$env:Path"

The scan is saved as JSON in the `collection` folder by default:
.\build-ucrt64\RetroTransfer.exe "any directory"

To choose a different JSON filename inside `collection`, provide it as the second argument:
.\build-ucrt64\RetroTransfer.exe "any directory" "project-name.json"

Each saved JSON file contains a directory tree. Every directory has `name`, `path`,
`directories`, and `files` fields. Every file records its `name`, `path`, `size`,
`type`, and `modificationTime`.

~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
