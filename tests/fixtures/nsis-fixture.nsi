; Generate with NSIS 3.13: makensis nsis-fixture.nsi
; Compiler/source: https://nsis.sourceforge.io/Download (unmodified distribution).
Name "MilkZip fixture"
OutFile "nsis.exe"
SetCompressor /SOLID lzma
InstallDir "$TEMP\MilkZip-fixture"
Section
SetOutPath "$INSTDIR"
File "nsis-payload.txt"
SectionEnd
