#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

VALID_PROXY_SCHEMES = ("http://", "https://", "socks5://", "socks5h://")


def fail(message: str) -> int:
    print(f"error: {message}", file=sys.stderr)
    return 1


def read_local_properties(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    if not path.is_file():
        return values
    for raw in path.read_text(encoding="utf-8-sig").splitlines():
        line = raw.strip()
        if not line or line.startswith(("#", "!")):
            continue
        match = re.match(r"^([^=:]+?)\s*[=:]\s*(.*)$", line)
        if not match:
            continue
        key = match.group(1).strip()
        value = match.group(2).strip().replace("\\:", ":").replace("\\=", "=").replace("\\\\", "\\")
        values[key] = value
    return values


def resolve_proxy(cli_proxy: str | None, workspace: Path, environ: dict[str, str] | os._Environ[str] | None = None) -> str | None:
    if cli_proxy is not None:
        candidate = cli_proxy.strip()
        if not candidate or not candidate.startswith(VALID_PROXY_SCHEMES):
            raise ValueError("--proxy must be an explicit HTTP(S) or SOCKS5 proxy URL.")
        return candidate
    local_proxy = read_local_properties(workspace / "local.properties").get("proxy", "").strip()
    if local_proxy:
        if not local_proxy.startswith(VALID_PROXY_SCHEMES):
            raise ValueError("local.properties proxy must be an explicit HTTP(S) or SOCKS5 proxy URL.")
        return local_proxy
    env = os.environ if environ is None else environ
    for name in ("HTTPS_PROXY", "https_proxy", "HTTP_PROXY", "http_proxy", "ALL_PROXY", "all_proxy"):
        value = env.get(name, "").strip()
        if value:
            if not value.startswith(VALID_PROXY_SCHEMES):
                raise ValueError(f"{name} must be an explicit HTTP(S) or SOCKS5 proxy URL.")
            return value
    return None


def run_git(
    executable: str,
    directory: Path,
    arguments: list[str],
    proxy: str | None,
    capture: bool = False,
) -> subprocess.CompletedProcess[str]:
    environment = os.environ.copy()
    # Repository discovery must not escape the managed checkout.
    for name in (
        "GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_COMMON_DIR",
        "GIT_OBJECT_DIRECTORY", "GIT_ALTERNATE_OBJECT_DIRECTORIES",
    ):
        environment.pop(name, None)
    environment["GIT_TERMINAL_PROMPT"] = "0"
    command = [executable]
    if proxy:
        environment.update({
            "HTTP_PROXY": proxy,
            "HTTPS_PROXY": proxy,
            "ALL_PROXY": proxy,
            "http_proxy": proxy,
            "https_proxy": proxy,
            "all_proxy": proxy,
            "NO_PROXY": "",
            "no_proxy": "",
        })
        command.extend(["-c", f"http.proxy={proxy}"])
    command.extend([
        "-c", "core.autocrlf=false",
        "-c", "submodule.recurse=false",
        "-C", str(directory),
        *arguments,
    ])
    return subprocess.run(
        command,
        env=environment,
        text=True,
        encoding="utf-8",
        errors="replace",
        capture_output=capture,
        check=False,
    )


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Fetch the pinned RadRay source without submodules or dependency builds."
    )
    parser.add_argument("command", nargs="?", choices=("restore", "status"), default="restore")
    parser.add_argument("--proxy", default=None, help="Optional HTTP(S) or SOCKS5 proxy URL")
    options = parser.parse_args()

    workspace = Path(__file__).resolve().parent.parent
    try:
        proxy = resolve_proxy(options.proxy, workspace)
    except ValueError as error:
        return fail(str(error))

    lock_path = workspace / "native" / "radray.lock.json"
    if not lock_path.is_file():
        return fail(f"Missing dependency lock: {lock_path}")
    lock = json.loads(lock_path.read_text(encoding="utf-8"))
    if not isinstance(lock, dict) or lock.get("schemaVersion") != 1:
        return fail("Unsupported native/radray.lock.json schema.")
    repository = lock.get("repository")
    revision = lock.get("commit")
    if not isinstance(repository, str) or not repository.startswith("https://"):
        return fail("The dependency repository must be an HTTPS URL.")
    if not isinstance(revision, str) or re.fullmatch(r"[0-9a-fA-F]{40}", revision) is None:
        return fail("The dependency commit must be a full 40-character Git commit ID.")
    revision = revision.lower()

    executable = shutil.which("git")
    if executable is None:
        return fail("Git is required. Install Git and make it available on PATH.")

    checkout = workspace / "external" / "RadRay"
    if checkout.resolve() != checkout or not checkout.resolve().is_relative_to(workspace):
        return fail(f"The managed checkout must not be redirected by a symlink or junction: {checkout}")
    if checkout.exists() and not checkout.is_dir():
        return fail(f"The checkout path is not a directory: {checkout}")

    print(f"RadRay checkout: {checkout}", flush=True)
    print(f"Pinned commit: {revision}", flush=True)
    if not (checkout / ".git").exists():
        if options.command == "status":
            return fail("RadRay has not been fetched. Run this script with restore.")
        if checkout.exists() and any(checkout.iterdir()):
            return fail("The destination is nonempty and is not a managed Git checkout. Nothing was changed.")
        checkout.mkdir(parents=True, exist_ok=True)
        if run_git(executable, checkout, ["init", "--quiet"], proxy).returncode != 0:
            return fail("Could not initialize the RadRay checkout.")
        if run_git(executable, checkout, ["remote", "add", "origin", repository], proxy).returncode != 0:
            return fail("Could not record the RadRay origin. The incomplete checkout was preserved.")

    git_directory = checkout / ".git"
    if not git_directory.is_dir() or git_directory.resolve() != git_directory:
        return fail("Expected an ordinary local .git directory, not a worktree, submodule or redirected Git directory.")
    top = run_git(executable, checkout, ["rev-parse", "--show-toplevel"], proxy, True)
    if top.returncode != 0 or Path(top.stdout.strip()).resolve() != checkout:
        return fail("The destination is not its own Git repository.")
    remote = run_git(executable, checkout, ["remote", "get-url", "origin"], proxy, True)
    if remote.returncode != 0 or remote.stdout.strip() != repository:
        return fail("The checkout origin does not match the lock file. No remote was replaced.")

    status = run_git(executable, checkout, ["status", "--porcelain", "--untracked-files=normal"], proxy, True)
    if status.returncode != 0:
        return fail("Could not read the checkout status.")
    if status.stdout.strip():
        print(status.stdout, end="")
        return fail("RadRay has local changes or untracked files. Preserve them before restoring; no reset or clean was performed.")

    head = run_git(executable, checkout, ["rev-parse", "--verify", "HEAD"], proxy, True)
    current = head.stdout.strip().lower() if head.returncode == 0 else None
    if current == revision:
        print("RadRay already matches the lock. No download or checkout needed.")
        return 0
    if options.command == "status":
        return fail(f"Checkout revision is {current or 'not yet checked out'}; restore is required.")
    if current is not None:
        branch = run_git(executable, checkout, ["symbolic-ref", "--quiet", "HEAD"], proxy, True)
        if branch.returncode == 0:
            return fail("The checkout is on a local branch. Switch it to detached HEAD explicitly before restoring a different revision.")
        if branch.returncode != 1:
            return fail("Could not determine whether the checkout is on a branch.")

    available = run_git(executable, checkout, ["cat-file", "-t", revision], proxy, True)
    if available.returncode != 0:
        via = f" through proxy {proxy}" if proxy else ""
        print(f"Fetching the pinned RadRay commit{via}...", flush=True)
        fetched = run_git(
            executable, checkout,
            ["fetch", "--no-tags", "--no-recurse-submodules", "--depth=1", "origin", revision],
            proxy,
        )
        if fetched.returncode != 0:
            return fail("Fetch failed. The checkout was preserved; fix the connection and rerun restore.")
        available = run_git(executable, checkout, ["cat-file", "-t", revision], proxy, True)
    if available.returncode != 0 or available.stdout.strip() != "commit":
        return fail("The locked object is not an available Git commit.")

    restored = run_git(
        executable, checkout,
        ["-c", "advice.detachedHead=false", "checkout", "--detach", revision],
        proxy,
    )
    if restored.returncode != 0:
        return fail("Checkout failed. No forced checkout, reset or clean was performed.")
    installed = run_git(executable, checkout, ["rev-parse", "--verify", "HEAD"], proxy, True)
    if installed.returncode != 0 or installed.stdout.strip().lower() != revision:
        return fail("The restored checkout does not match the pinned commit.")
    print("RadRay source is ready. Third-party dependencies are not fetched or built by this script.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
