#!/bin/bash
# CLMA 项目快速备份脚本
# 用法: ./scripts/backup.sh [备份名称]

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BACKUP_NAME="${1:-backup_$(date +%Y%m%d_%H%M%S)}"
BACKUP_FILE="${SCRIPT_DIR}/${BACKUP_NAME}.tar.gz"

echo "📦 备份到: ${BACKUP_FILE}"
echo ""

cd "${SCRIPT_DIR}"

tar --exclude='venv/' \
    --exclude='build/' \
    --exclude='__pycache__/' \
    --exclude='.pytest_cache/' \
    --exclude='.hermes/' \
    --exclude='*.tar.gz' \
    --exclude='tools/sandbox/script_*' \
    -czf "${BACKUP_FILE}" \
    CMakeLists.txt \
    README.md \
    .gitignore \
    config/ \
    docs/ \
    include/ \
    plugins/ \
    python_bindings/ \
    python_interface/ \
    scripts/ \
    src/ \
    tests/ \
    tools/ \
    run_webui.sh

echo "✅ 备份完成: ${BACKUP_FILE}"
echo "   大小: $(du -h "${BACKUP_FILE}" | cut -f1)"
echo ""
echo "  恢复命令: tar -xzf ${BACKUP_FILE}"
