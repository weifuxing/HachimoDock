"""Clean-checkout SDK input gate; no downloads, PlatformIO or firmware builds."""
import importlib.util
from pathlib import Path
import subprocess
import sys

import pytest


@pytest.mark.parametrize("state", ["tracked", "ignored", "deleted"])
def test_sdk_defaults_must_survive_a_clean_checkout(tmp_path, monkeypatch, state):
    tools = Path(__file__).resolve().parents[1] / "tools"
    monkeypatch.syspath_prepend(str(tools))
    spec = importlib.util.spec_from_file_location("ci_sdk_gate", tools / "prepare_ci_sdk.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    subprocess.run(["git", "init", "-q", str(tmp_path)], check=True)
    missing = "sdkconfig.wifi-settings.defaults"
    for name in module.PROJECT_INPUTS:
        path = tmp_path / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("# SDK input fixture\n")
        if state != "ignored" or name != missing:
            subprocess.run(["git", "add", "--", name], cwd=tmp_path, check=True)
    if state == "ignored":
        (tmp_path / ".gitignore").write_text(missing + "\n")
    if state == "deleted":
        (tmp_path / missing).unlink()
    if state == "tracked":
        module.check_checkout_inputs(tmp_path)
        return
    # Exercise main ordering too: the missing input must fail before WAMR or PIO.
    monkeypatch.setattr(module, "__file__", str(tmp_path / "tools/prepare_ci_sdk.py"))
    monkeypatch.setattr(sys, "argv", ["prepare", "--v3-core", str(tmp_path / "v3-core")])
    monkeypatch.setenv("GITHUB_ENV", str(tmp_path / "ci-env"))
    def no_download(*args, **kwargs):
        pytest.fail("Dependency preparation ran before checking checkout inputs")
    monkeypatch.setattr(module, "prepare_wamr", no_download)
    with pytest.raises(SystemExit, match=missing):
        module.main()
    assert not (tmp_path / "ci-env").exists()


@pytest.mark.parametrize("existing", [False, True])
@pytest.mark.parametrize("fails", [False, True])
def test_sdk_configure_restores_exact_input_and_keeps_generated_context(tmp_path, monkeypatch, existing, fails):
    tools = Path(__file__).resolve().parents[1] / "tools"
    monkeypatch.syspath_prepend(str(tools))
    spec = importlib.util.spec_from_file_location("ci_sdk_restore", tools / "prepare_ci_sdk.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    target = "esp32_p4_program_public_candidate_v3"
    config = tmp_path / f"sdkconfig.{target}"
    original = b"# Reviewed input\r\nCONFIG_FLASH_SIZE=32\r\n"
    if existing:
        config.write_bytes(original)
    generated = tmp_path / ".pio/build" / target / "config/sdkconfig.h"
    generated.parent.mkdir(parents=True)
    sdk_header = b"/* actual generated context remains available */\n"
    def configure():
        with module.preserve_sdkconfig(tmp_path, target):
            config.write_bytes(b"# normalized by SDK\nCONFIG_FLASH_SIZE=32\n")
            generated.write_bytes(sdk_header)
            if fails:
                raise subprocess.CalledProcessError(1, ["platformio", "compiledb"])
    if fails:
        with pytest.raises(subprocess.CalledProcessError):
            configure()
    else:
        configure()
    assert config.exists() == existing
    if existing:
        assert config.read_bytes() == original
    assert generated.read_bytes() == sdk_header
