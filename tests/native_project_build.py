"""Check native module selection across projects in one reused CMake cache."""
import pathlib
import subprocess
import sys
import tempfile

engine = pathlib.Path(__file__).resolve().parents[1]
cmake = sys.argv[1] if len(sys.argv) > 1 else "cmake"
with tempfile.TemporaryDirectory(prefix="rts-native-projects-") as tmp:
    root = pathlib.Path(tmp)
    src, build = root / "engine", root / "build"
    (src / "modules/rts").mkdir(parents=True)
    # The real selection/registration code is exercised; no renderer/toolchain is needed.
    (src / "modules/rts/RtsNative.cpp").write_text("")
    (src / "stub.cpp").write_text("")
    module_cmake = (engine / "modules/rts/native/Build.cmake").as_posix()
    (src / "CMakeLists.txt").write_text(
        'cmake_minimum_required(VERSION 3.22)\nproject(NativeIsolation LANGUAGES CXX)\n'
        'add_library(rts_game STATIC stub.cpp)\ninclude("' + module_cmake + '")\n')
    for name in ("a", "b"):
        native = root / name / "native"
        native.mkdir(parents=True)
        (native / "Module.cpp").write_text("")
        (native / "CMakeLists.txt").write_text(
            'rts_native_module(ID local VERSION 1 ENTRY create_' + name + ' SOURCES Module.cpp)\n')
    (root / "empty").mkdir()

    def configure(project, *args):
        subprocess.run([cmake, "-S", str(src), "-B", str(build),
                        "-DRTS_CONTENT_DIR=" + str(root / project), *args],
                       check=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        return (build / "gen/RtsNativeRegistry.cpp").read_text()

    a = configure("a")
    assert "create_a" in a and "create_b" not in a
    b = configure("b")
    assert "create_b" in b and "create_a" not in b
    empty = configure("empty")
    assert "create_a" not in empty and "create_b" not in empty
    override = configure("b", "-DRTS_NATIVE_DIR=" + str(root / "a/native"))
    assert "create_a" in override and "create_b" not in override
    # This is the cache reset supplied by rtsbuild for the current game project.
    reset = configure("b", "-DRTS_NATIVE_DIR=")
    assert "create_b" in reset and "create_a" not in reset
    print("Native modules: project isolation and cache reset passed")
