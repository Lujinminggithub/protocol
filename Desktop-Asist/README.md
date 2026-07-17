# 🧚 AI小精灵 - 桌面AI助手

一个功能丰富的桌面AI小精灵托盘应用程序,集成了多种实用工具。

## ✨ 功能特性

| 模块 | 功能 |
|------|------|
| 🤖 **AI助手** | 智能聊天界面,支持与AI对话 |
| 📅 **日历** | 日程管理,支持添加和查看日程 |
| 🔍 **文件搜索** | 本地文件快速搜索,支持通配符 |
| 🎨 **AIGC** | AI生成内容(文本/代码/图片描述) |
| ⚡ **Hermes** | 系统监控工具,资源监控与快捷操作 |

## 📦 安装

### 环境要求
- Python 3.10+
- Windows 10/11 (推荐)

### 安装依赖

```bash
pip install -r requirements.txt
```

如果需要系统监控功能,额外安装:

```bash
pip install psutil
```

## 🚀 运行

### 方式一: 直接运行
```bash
python main.py
```

### 方式二: 使用启动脚本
双击 `run.bat` 文件

## 📁 项目结构

```
Desktop-Asist/
├── main.py                 # 主入口
├── requirements.txt        # 依赖列表
├── run.bat                 # Windows启动脚本
├── assets/
│   └── icons/              # 图标资源
└── src/
    ├── __init__.py
    ├── styles.py           # 深色主题样式表
    ├── tray.py             # 托盘图标模块
    ├── window.py           # 主窗口
    ├── ai_panel.py         # AI助手面板
    ├── calendar_panel.py   # 日历面板
    ├── search_panel.py     # 文件搜索面板
    ├── aigc_panel.py       # AIGC生成面板
    └── hermes_panel.py     # Hermes系统工具面板
```

## 🎨 界面预览

- **深色主题**: 采用 Catppuccin Mocha 配色方案
- **托盘图标**: 最小化后驻留系统托盘,双击恢复
- **侧边栏导航**: 左侧导航栏快速切换功能模块

## 🔧 扩展开发

### 接入真实AI接口

在 `src/ai_panel.py` 中,修改消息发送逻辑:

```python
# 替换 _on_send 中的调用
def _on_send(self):
    text = self.input_field.text().strip()
    if not text:
        return
    self.input_field.clear()
    self._add_message(text, is_user=True)
    # 调用AI接口
    asyncio.create_task(self._fetch_ai_response(text))
```

### 自定义主题

编辑 `src/styles.py` 中的 `STYLE_SHEET` 字符串即可修改整个应用的UI样式。

## 📝 快捷键

| 快捷键 | 功能 |
|--------|------|
| `Enter` | 在AI助手中发送消息 |
| `双击托盘图标` | 打开/恢复窗口 |
| `ESC` | 最小化窗口 |

## 📄 License

MIT
