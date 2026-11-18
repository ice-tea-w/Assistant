# -*- coding: utf-8 -*-
"""一次性脚本：为 PyCharm 配置 file_guard 工程解释器与运行配置。"""
from __future__ import annotations

import shutil
import uuid
import xml.etree.ElementTree as ET
from pathlib import Path

PROJECT = Path(r"E:\PyCharm 2026.2.3\projects\file_guard")
JDK_TABLE = Path.home() / "AppData/Roaming/JetBrains/PyCharm2026.2/options/jdk.table.xml"
SDK_NAME = "Python 3.12 (file_guard)"
SDK_UUID = str(uuid.uuid4())

sdk_block = f'''    <jdk version="2">
      <name value="{SDK_NAME}" />
      <type value="Python SDK" />
      <version value="Python 3.12.10" />
      <homePath value="$APPLICATION_HOME_DIR$/projects/file_guard/.venv/Scripts/python.exe" />
      <roots>
        <classPath>
          <root type="composite">
            <root url="file://$USER_HOME$/AppData/Local/Programs/Python/Python312/DLLs" type="simple" />
            <root url="file://$USER_HOME$/AppData/Local/Programs/Python/Python312/Lib" type="simple" />
            <root url="file://$USER_HOME$/AppData/Local/Programs/Python/Python312" type="simple" />
            <root url="file://$APPLICATION_HOME_DIR$/projects/file_guard/.venv" type="simple" />
            <root url="file://$APPLICATION_HOME_DIR$/projects/file_guard/.venv/Lib/site-packages" type="simple" />
            <root url="file://$APPLICATION_HOME_DIR$/plugins/python-ce/helpers/typeshed/stdlib" type="simple" />
          </root>
        </classPath>
        <sourcePath>
          <root type="composite" />
        </sourcePath>
      </roots>
      <additional ASSOCIATED_PROJECT_PATH="$APPLICATION_HOME_DIR$/projects/file_guard" WORKING_DIRECTORY="E:\\PyCharm 2026.2.3\\projects\\file_guard" SDK_UUID="{SDK_UUID}">
        <setting name="FLAVOR_ID" value="VirtualEnvSdkFlavor" />
        <setting name="FLAVOR_DATA" value="{{}}" />
      </additional>
    </jdk>
'''


def setup_jdk_table() -> str:
    if not JDK_TABLE.exists():
        return "jdk.table.xml 不存在，跳过（PyCharm 首次启动后会自动创建）"
    text = JDK_TABLE.read_text(encoding="utf-8")
    if SDK_NAME in text:
        return "解释器已注册，跳过"
    backup = JDK_TABLE.with_suffix(".xml.bak-file_guard")
    shutil.copy2(JDK_TABLE, backup)
    marker = "  </component>\n</application>"
    if marker not in text:
        return "jdk.table.xml 结构不符合预期，未修改（已备份于 " + str(backup) + "）"
    text = text.replace(marker, sdk_block + marker, 1)
    JDK_TABLE.write_text(text, encoding="utf-8")
    # 校验 XML 仍然合法
    ET.parse(JDK_TABLE)
    return f"已注册解释器 {SDK_NAME}（备份：{backup.name}）"


def setup_misc() -> str:
    path = PROJECT / ".idea" / "misc.xml"
    content = f'''<?xml version="1.0" encoding="UTF-8"?>
<project version="4">
  <component name="ProjectRootManager" version="2" project-jdk-name="{SDK_NAME}" project-jdk-type="Python SDK" />
</project>
'''
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")
    ET.parse(path)
    return "已写入 .idea/misc.xml（项目解释器）"


def setup_run_configs() -> str:
    run_dir = PROJECT / ".idea" / "runConfigurations"
    run_dir.mkdir(parents=True, exist_ok=True)
    demo = '''<component name="ProjectRunConfigurationManager">
  <configuration default="false" name="demo（加密→检测→审计→备份）" type="PythonConfigurationType" factoryName="Python" nameIsGenerated="false">
    <module name="file_guard" />
    <option name="ENV_FILES" value="" />
    <option name="INTERPRETER_OPTIONS" value="" />
    <option name="PARENT_ENVS" value="true" />
    <envs>
      <env name="PYTHONIOENCODING" value="utf-8" />
    </envs>
    <option name="SDK_HOME" value="" />
    <option name="WORKING_DIRECTORY" value="$PROJECT_DIR$" />
    <option name="IS_MODULE_SDK" value="true" />
    <option name="ADD_CONTENT_ROOTS" value="true" />
    <option name="ADD_SOURCE_ROOTS" value="true" />
    <option name="SCRIPT_NAME" value="$PROJECT_DIR$/examples/demo.py" />
    <option name="PARAMETERS" value="" />
    <option name="SHOW_COMMAND_LINE" value="false" />
    <option name="EMULATE_TERMINAL" value="false" />
    <option name="MODULE_MODE" value="false" />
    <option name="REDIRECT_INPUT" value="false" />
    <option name="INPUT_FILE" value="" />
    <method v="2" />
  </configuration>
</component>
'''
    (run_dir / "demo.xml").write_text(demo, encoding="utf-8")
    ET.parse(run_dir / "demo.xml")
    return "已添加运行配置 demo（examples/demo.py）"


if __name__ == "__main__":
    print(setup_jdk_table())
    print(setup_misc())
    print(setup_run_configs())
