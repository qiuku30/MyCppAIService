#!/usr/bin/env python3
"""Offline deployment contracts and CMake generation; never compile fake libraries."""
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]
AMQP_COMMIT = "839f74f2e7cc0f3f03458ebf943d23fc1f8c5462"


class DeployContracts(unittest.TestCase):
    def test_build_image_installs_pinned_linux_tcp_amqp(self):
        recipe = (ROOT / "deploy/Dockerfile.build").read_text()
        self.assertNotIn("SimpleAmqpClient", recipe)
        self.assertIn("CopernicaMarketingSoftware/AMQP-CPP", recipe)
        self.assertIn(AMQP_COMMIT, recipe)
        self.assertIn("-DAMQP-CPP_LINUX_TCP=ON", recipe)
        self.assertIn("TARGETARCH", recipe)
        self.assertIn("aarch64", (ROOT / "deploy/build-ort.sh").read_text())

    def test_docker_recipes_have_valid_instruction_and_shell_syntax(self):
        for name in ("Dockerfile.build", "Dockerfile"):
            lines = (ROOT / "deploy" / name).read_text().splitlines()
            pending = ""
            instructions = []
            for line in lines:
                if not pending and (not line.strip() or line.lstrip().startswith("#")):
                    continue
                continued = line.rstrip().endswith("\\")
                pending += line.rstrip()[:-1] + " " if continued else line
                if not continued:
                    instructions.append(pending)
                    pending = ""
            self.assertFalse(pending, name + " has an unfinished continuation")
            for instruction in instructions:
                match = re.fullmatch(r"(FROM|ARG|RUN|COPY|WORKDIR|CMD)\s+(.+)", instruction)
                self.assertIsNotNone(match, f"{name}: invalid Docker instruction {instruction!r}")
                kind, body = match.groups()
                if kind == "RUN":
                    # BuildKit consumes mount options before invoking the shell.
                    body = re.sub(r"^(?:--mount=\S+\s+)+", "", body)
                    result = subprocess.run(["sh", "-n", "-c", body], text=True, capture_output=True)
                    self.assertEqual(result.returncode, 0, name + ": " + result.stderr)
                elif kind == "CMD":
                    self.assertIsInstance(json.loads(body), list)

    def test_image_assets_are_documented_as_an_opt_in_override(self):
        core = (ROOT / "deploy/docker-compose.yml").read_text()
        self.assertNotIn("IMAGE_MODEL_HOST_FILE", core)
        self.assertNotIn("IMAGE_LABELS_HOST_FILE", core)
        documentation = (ROOT / "deploy/README.md").read_text()
        self.assertIn("-f docker-compose.yml -f docker-compose.image.yml", documentation)
        self.assertIn("IMAGE_MODEL_HOST_FILE", documentation)
        self.assertIn("IMAGE_LABELS_HOST_FILE", documentation)
        example = (ROOT / "deploy/.env.example").read_text()
        self.assertNotRegex(example, r"(?m)^IMAGE_(MODEL|LABELS)_HOST_FILE=")

    @unittest.skipUnless(shutil.which("ruby"), "Ruby YAML parser is optional")
    def test_optional_image_asset_bindings_match_existing_cpp_paths(self):
        override = ROOT / "deploy/docker-compose.image.yml"
        self.assertTrue(override.is_file(), "optional image asset override is missing")
        result = subprocess.run(
            ["ruby", "-rjson", "-ryaml", "-e",
             "puts JSON.generate(YAML.load_file(ARGV.fetch(0)))", str(override)],
            text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        service = json.loads(result.stdout)["services"]["ai-service"]
        self.assertEqual(set(service), {"volumes"}, "override only adds asset mounts")
        mounts = {entry["target"]: entry for entry in service["volumes"]}
        expected = {
            "/root/models/mobilenetv2/mobilenetv2-7.onnx": "IMAGE_MODEL_HOST_FILE",
            "/root/imagenet_classes.txt": "IMAGE_LABELS_HOST_FILE",
        }
        self.assertEqual(set(mounts), set(expected))
        for target, variable in expected.items():
            mount = mounts[target]
            self.assertEqual(mount["type"], "bind")
            self.assertTrue(mount["read_only"])
            self.assertFalse(mount["bind"]["create_host_path"])
            self.assertTrue(mount["source"].startswith("$" + "{" + variable + ":?"),
                            "asset source must require an explicit host file")
        handler = (ROOT / "AIApps/ChatServer/src/handlers/AIUploadSendHandler.cpp").read_text()
        recognizer = (ROOT / "AIApps/ChatServer/include/AIUtil/ImageRecognizer.h").read_text()
        self.assertIn("/root/models/mobilenetv2/mobilenetv2-7.onnx", handler)
        self.assertIn("/root/imagenet_classes.txt", recognizer)

    def test_build_paths_are_portable(self):
        for relative in ("CMakeLists.txt", "run.sh", "deploy/Dockerfile"):
            text = (ROOT / relative).read_text()
            self.assertNotIn("/home/qiuku", text, relative)
            self.assertNotIn("/usr/lib/x86_64-linux-gnu", text, relative)
            self.assertNotIn("httpserver_vsersion2", text, relative)

    def test_container_models_use_container_paths(self):
        compose = (ROOT / "deploy/docker-compose.yml").read_text()
        self.assertRegex(compose, r"GEMMA_MODEL_PATH: /opt/gemma-models/")
        self.assertRegex(compose, r"GEMMA_TOKENIZER_PATH: /opt/gemma-models/")
        self.assertIn("GEMMA_MODELS_HOST_DIR", compose)
        self.assertIn("GEMMA_BUILD_HOST_DIR", compose)
        self.assertIn("GEMMA_SOURCE_HOST_DIR", compose)
        self.assertIn("/opt/gemma-models:ro", compose)
        example = (ROOT / "deploy/.env.example").read_text()
        self.assertNotIn("/home/qiuku", example)
        self.assertNotRegex(example, r"(?m)^GEMMA_(MODEL|TOKENIZER)_PATH=")

    def test_fresh_selected_database_bootstrap_preserves_p1_schema(self):
        compose = (ROOT / "deploy/docker-compose.yml").read_text()
        self.assertIn("./init.sql:/docker-entrypoint-initdb.d/01-schema.sql:ro", compose)
        self.assertIn("MYSQL_DATABASE: ${MYSQL_DATABASE", compose)
        self.assertIn("DB_NAME: ${MYSQL_DATABASE", compose)
        sql = (ROOT / "deploy/init.sql").read_text()
        self.assertNotRegex(sql, r"(?im)^\s*(CREATE DATABASE|USE\s)")
        self.assertIn("CREATE TABLE IF NOT EXISTS users", sql)
        self.assertIn("password VARCHAR(255)", sql)
        self.assertIn("message_id VARCHAR(64)", sql)
        self.assertIn("UNIQUE KEY uq_chat_message_id (message_id)", sql)

    def test_legacy_dockerfile_reuses_authoritative_image(self):
        wrapper = (ROOT / "deploy/Dockerfile").read_text()
        self.assertIn("mycppaiservice-build:local", wrapper)
        self.assertIn("FROM ${BUILD_IMAGE}", wrapper)
        self.assertNotIn("COPY *.so", wrapper)
        self.assertIn("/workspace/deploy/build-and-run.sh", wrapper)

    def test_local_launcher_preserves_environment_and_arguments(self):
        with tempfile.TemporaryDirectory(prefix="mycpp p2 launch ") as tmp:
            build = Path(tmp) / "chosen build"
            build.mkdir()
            binary = build / "http_server"
            binary.write_text('#!/bin/sh\nprintf "%s\\n" "$PWD" "$DB_NAME" "$DASHSCOPE_API_KEY" "$@"\n')
            binary.chmod(0o755)
            env = dict(os.environ, BUILD_DIR=str(build), DB_NAME="chosen_database",
                       DASHSCOPE_API_KEY="fixture_key")
            result = subprocess.run(["bash", str(ROOT / "run.sh"), "-p", "9001"],
                                    cwd=tmp, env=env, text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.splitlines(),
                             [str(build), "chosen_database", "fixture_key", "-p", "9001"])

    def test_build_launcher_configures_then_builds_and_runs(self):
        script = ROOT / "deploy/build-and-run.sh"
        self.assertTrue(script.is_file(), "container build entrypoint is missing")
        with tempfile.TemporaryDirectory(prefix="mycpp p2 entrypoint ") as tmp:
            work = Path(tmp)
            build = work / "fresh build"
            commands = work / "bin"
            commands.mkdir()
            cmake = commands / "cmake"
            cmake.write_text('#!/bin/sh\nprintf "%s\\n" "$@" >> "$P2_COMMAND_LOG"\n')
            cmake.chmod(0o755)
            build.mkdir()
            binary = build / "http_server"
            binary.write_text('#!/bin/sh\nprintf "%s\\n" "$PWD" "$@"\n')
            binary.chmod(0o755)
            env = dict(os.environ, BUILD_DIR=str(build), BUILD_JOBS="3",
                       PATH=f"{commands}:{os.environ['PATH']}",
                       P2_COMMAND_LOG=str(work / "commands.txt"))
            result = subprocess.run(["sh", str(script), "-p", "9002"], cwd=tmp,
                                    env=env, text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.splitlines(), [str(build), "-p", "9002"])
            args = (work / "commands.txt").read_text().splitlines()
            self.assertEqual(args[:4], ["-S", str(ROOT), "-B", str(build)])
            self.assertIn("--build", args)
            self.assertEqual(args[-2:], ["--parallel", "3"])


class CMakeGeneration(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="mycpp p2 cmake ")
        self.addCleanup(self.tmp.cleanup)
        self.base = Path(self.tmp.name)
        self.prefix = self.base / "dependency prefix"
        self.include = self.prefix / "include"
        self.lib = self.prefix / "lib"
        self.modules = self.base / "modules"
        self.gemma_source = self.base / "gemma source"
        self.gemma_build = self.base / "gemma build"
        for path in (self.include, self.lib, self.modules, self.gemma_source, self.gemma_build):
            path.mkdir(parents=True)
        headers = ["mysql_driver.h", "mysql/mysql.h", "muduo/net/TcpServer.h", "amqpcpp.h",
                   "amqpcpp/linux_tcp/tcpconnection.h", "onnxruntime_cxx_api.h", "nlohmann/json.hpp"]
        for relative in headers:
            path = self.include / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.touch()
        for name in ("mysqlcppconn", "mysqlclient", "muduo_net", "muduo_base", "amqpcpp", "onnxruntime"):
            (self.lib / f"lib{name}.a").touch()
        for path in (self.gemma_source / "gemma/gemma.h",
                     self.gemma_build / "libgemma.a",
                     self.gemma_build / "_deps/highway-build/libhwy.a",
                     self.gemma_build / "_deps/highway-build/libhwy_contrib.a",
                     self.gemma_build / "_deps/highway-src/hwy/base.h",
                     self.gemma_build / "_deps/sentencepiece-build/src/libsentencepiece.a",
                     self.gemma_build / "_deps/sentencepiece-src/src/sentencepiece_processor.h"):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.touch()
        for package, targets in (("OpenSSL", ["OpenSSL::SSL", "OpenSSL::Crypto"]),
                                 ("CURL", ["CURL::libcurl"]), ("OpenCV", ["fixture_opencv"])):
            content = f"set({package}_FOUND TRUE)\n"
            for target in targets:
                content += f"add_library({target} INTERFACE IMPORTED)\n"
            if package == "OpenCV":
                content += f'set(OpenCV_LIBS fixture_opencv)\nset(OpenCV_INCLUDE_DIRS "{self.include}")\n'
            (self.modules / f"Find{package}.cmake").write_text(content)

    def configure(self):
        build = self.base / "application build"
        command = ["cmake", "-S", str(ROOT), "-B", str(build),
                   f"-DCMAKE_MODULE_PATH={self.modules}",
                   f"-DCMAKE_PREFIX_PATH={self.prefix}",
                   f"-DCMAKE_FIND_ROOT_PATH={self.prefix}",
                   "-DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY",
                   "-DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY",
                   f"-DGEMMA_SOURCE_DIR={self.gemma_source}",
                   f"-DGEMMA_BUILD_DIR={self.gemma_build}",
                   "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"]
        env = dict(os.environ, GEMMA_SOURCE_DIR="/missing/env-source",
                   GEMMA_BUILD_DIR="/missing/env-build")
        return subprocess.run(command, env=env, text=True, capture_output=True), build

    def test_nonstandard_dependency_prefix_and_cache_override_generate(self):
        result, build = self.configure()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        commands = (build / "compile_commands.json").read_text()
        self.assertTrue(json.loads(commands), "expected generated source compile commands")
        self.assertIn(str(self.gemma_source), commands)
        self.assertIn(str(self.gemma_build), commands)
        self.assertNotIn("/missing/env-", commands)
        self.assertNotIn("/home/qiuku", commands)
        links = list(build.rglob("link.txt"))
        self.assertTrue(links, "expected generated Makefile linker command")
        link = links[0].read_text()
        self.assertIn(str(self.lib / "libamqpcpp.a"), link)
        self.assertIn(str(self.gemma_build / "libgemma.a"), link)
        self.assertNotIn("/usr/lib/x86_64-linux-gnu", link)

    def test_missing_amqp_library_fails_at_configure(self):
        (self.lib / "libamqpcpp.a").unlink()
        result, _ = self.configure()
        self.assertNotEqual(result.returncode, 0, "missing AMQP-CPP must fail during configure")
        self.assertIn("AMQPCPP_LIBRARY", result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
