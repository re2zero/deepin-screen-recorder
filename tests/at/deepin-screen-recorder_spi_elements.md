# AT-SPI 元素清单 — deepin-screen-recorder（截图录屏）

- 生成时间: 2026-08-31 18:28:18
- 源码 commit: `dffd14aadf3a342d47065db5f38f03ea567c05d9 2026-08-31 13:11:30 +0800`
- 源码路径: `deepin-screen-recorder`

## 覆盖率汇总

| 语言 | 已命名 (ok) | 缺失 (gap) | 应命名 (total) | 覆盖率 |
|------|-----------|-----------|---------------|--------|
| C++ | 91 | 0 | 91 | 100.0% |
| QML | 0 | 0 | 0 | 0.0% |
| **合计** | **91** | **0** | **91** | **100.0%** |

- 阈值判定: PASS ✅ (≥ 80%)

## AT 用例覆盖率

- 用例数: 24 个 (suite 文件 3 个)
- 扫描交互控件总数: 91
- 用例覆盖引用: 0
- 覆盖率: 0.0%
- 判定: FAIL ❌

## C++ 已命名交互控件 (SPI 元素)

| # | 变量名 | 类型 | 源文件 | 行号 | objectName | accessibleName | 角色 |
|---|--------|------|--------|------|------------|----------------|------|
| 1 | m_menu | DMenu * | src/menucontroller/menucontroller.cpp | 34 | Menu | Menu | menu |
| 2 | m_unDoAct | QAction * | src/menucontroller/menucontroller.cpp | 35 | UnDoAct |  | push button |
| 3 | m_saveAct | QAction * | src/menucontroller/menucontroller.cpp | 36 | SaveAct |  | push button |
| 4 | m_closeAct | QAction * | src/menucontroller/menucontroller.cpp | 37 | CloseAct |  | push button |
| 5 | m_menu | DMenu * | pin_screenshots/ui/menucontroller.cpp | 46 | Menu | Menu | menu |
| 6 | m_saveAct | QAction * | pin_screenshots/ui/menucontroller.cpp | 50 | SaveAct |  | push button |
| 7 | m_closeAct | QAction * | pin_screenshots/ui/menucontroller.cpp | 54 | CloseAct |  | push button |
| 8 | m_saveMenu | DMenu * | pin_screenshots/ui/pinsavemenumanager.cpp | 65 | SaveMenu | SaveMenu | menu |
| 9 | m_saveOptionGroup | QActionGroup * | pin_screenshots/ui/pinsavemenumanager.cpp | 67 | SaveOptionGroup |  | push button |
| 10 | m_askEveryTimeAction | QAction * | pin_screenshots/ui/pinsavemenumanager.cpp | 68 | AskEveryTimeAction |  | push button |
| 11 | m_specifiedLocationAction | QAction * | pin_screenshots/ui/pinsavemenumanager.cpp | 69 | SpecifiedLocationAction |  | push button |
| 12 | m_specifiedLocationSubMenu | DMenu * | pin_screenshots/ui/pinsavemenumanager.cpp | 71 | SpecifiedLocationSubMenu | SpecifiedLocationSubMenu | menu |
| 13 | m_customLocationGroup | QActionGroup * | pin_screenshots/ui/pinsavemenumanager.cpp | 72 | CustomLocationGroup |  | push button |
| 14 | m_desktopAction | QAction * | pin_screenshots/ui/pinsavemenumanager.cpp | 73 | DesktopAction |  | push button |
| 15 | m_picturesAction | QAction * | pin_screenshots/ui/pinsavemenumanager.cpp | 74 | PicturesAction |  | push button |
| 16 | m_saveToSpecialPathAction | QAction * | pin_screenshots/ui/pinsavemenumanager.cpp | 75 | SaveToSpecialPathAction |  | push button |
| 17 | m_changeSaveToSpecialPath | QAction * | pin_screenshots/ui/pinsavemenumanager.cpp | 76 | ChangeSaveToSpecialPath |  | push button |
| 18 | m_optionMenu | DMenu * | pin_screenshots/ui/subtoolwidget.cpp | 78 | OptionMenu | OptionMenu | menu |
| 19 | m_saveToSpecialPathMenu | DMenu * | pin_screenshots/ui/subtoolwidget.cpp | 82 |  |  | menu |
| 20 | m_saveToSpecialPathAction | QAction * | pin_screenshots/ui/subtoolwidget.cpp | 86 | SaveToSpecialPathAction |  | push button |
| 21 | m_changeSaveToSpecialPath | QAction * | pin_screenshots/ui/subtoolwidget.cpp | 90 |  |  | push button |
| 22 | m_saveGroup | QActionGroup * | pin_screenshots/ui/subtoolwidget.cpp | 108 | SaveGroup |  | push button |
| 23 | m_askEveryTimeAction | QAction * | pin_screenshots/ui/subtoolwidget.cpp | 109 |  |  | push button |
| 24 | m_explainButton | ToolButton * | src/widgets/aiassistantwidget.cpp | 38 | ExplainButton | ExplainButton | push button |
| 25 | m_summarizeButton | ToolButton * | src/widgets/aiassistantwidget.cpp | 39 | SummarizeButton | SummarizeButton | push button |
| 26 | m_translateButton | ToolButton * | src/widgets/aiassistantwidget.cpp | 40 | TranslateButton | TranslateButton | push button |
| 27 | m_askAIButton | ToolButton * | src/widgets/aiassistantwidget.cpp | 41 | AskAibutton | AskAibutton | push button |
| 28 | m_imgPrcThread | MajorImageProcessingThread * | src/widgets/camerawidget.cpp | 126 | MajorThread |  |  |
| 29 | m_colorButtonGroup | QButtonGroup * | src/widgets/colortoolwidget.cpp | 55 | ColorButtonGroup |  |  |
| 30 | m_fontSizeEdit | DLineEdit * | src/widgets/fontsizewidget.cpp | 39 | FontSizeEdit | FontSizeEdit | text |
| 31 | m_addSizeBtn | DPushButton * | src/widgets/fontsizewidget.cpp | 40 | AddSizeBtn | AddSizeBtn | push button |
| 32 | m_reduceSizeBtn | DPushButton * | src/widgets/fontsizewidget.cpp | 41 | ReduceSizeBtn | ReduceSizeBtn | push button |
| 33 | m_actionGroup | QButtonGroup * | src/widgets/imagemenu.cpp | 90 | ActionGroup |  |  |
| 34 | m_recordBtn | ToolButton * | src/widgets/maintoolwidget.cpp | 50 | RecordBtn | RecordBtn | push button |
| 35 | m_shotBtn | ToolButton * | src/widgets/maintoolwidget.cpp | 51 | ShotBtn | ShotBtn | push button |
| 36 | m_saveMenu | DMenu * | src/widgets/savemenumanager.cpp | 77 | SaveMenu | SaveMenu | menu |
| 37 | m_saveOptionGroup | QActionGroup * | src/widgets/savemenumanager.cpp | 80 | SaveOptionGroup |  | push button |
| 38 | m_askEachTimeAction | QAction * | src/widgets/savemenumanager.cpp | 81 | AskEachTimeAction |  | push button |
| 39 | m_specifiedLocationAction | QAction * | src/widgets/savemenumanager.cpp | 82 | SpecifiedLocationAction |  | push button |
| 40 | m_specifiedLocationSubMenu | DMenu * | src/widgets/savemenumanager.cpp | 85 | SpecifiedLocationSubMenu | SpecifiedLocationSubMenu | menu |
| 41 | m_locationGroup | QActionGroup * | src/widgets/savemenumanager.cpp | 86 | LocationGroup |  | push button |
| 42 | m_chooseOnSaveAction | QAction * | src/widgets/savemenumanager.cpp | 87 | ChooseOnSaveAction |  | push button |
| 43 | m_desktopAction | QAction * | src/widgets/savemenumanager.cpp | 88 | DesktopAction |  | push button |
| 44 | m_picturesAction | QAction * | src/widgets/savemenumanager.cpp | 89 | PicturesAction |  | push button |
| 45 | m_customPathAction | QAction * | src/widgets/savemenumanager.cpp | 90 | CustomPathAction |  | push button |
| 46 | m_updateOnSaveAction | QAction * | src/widgets/savemenumanager.cpp | 91 | UpdateOnSaveAction |  | push button |
| 47 | m_warmingIconButton | DIconButton * | src/widgets/scrollshottip.cpp | 132 | WarmingIconButton | WarmingIconButton |  |
| 48 | m_tipTextLable | DLabel * | src/widgets/scrollshottip.cpp | 136 | TipText |  | label |
| 49 | m_scrollShotHelp | DCommandLinkButton * | src/widgets/scrollshottip.cpp | 152 | scrollshot_tip_help_button | scrollshot_tip_help_button |  |
| 50 | m_scrollShotAdjust | DCommandLinkButton * | src/widgets/scrollshottip.cpp | 157 | scrollshot_tip_adjust_button | scrollshot_tip_adjust_button |  |
| 51 | m_shapeBtnGroup | QButtonGroup * | src/widgets/shapetoolwidget.cpp | 43 | ShapeBtnGroup |  |  |
| 52 | m_rectButton | ToolButton * | src/widgets/shapetoolwidget.cpp | 44 | RectButton | RectButton | push button |
| 53 | m_ovalButton | ToolButton * | src/widgets/shapetoolwidget.cpp | 45 | OvalButton | OvalButton | push button |
| 54 | m_thicknessBtnGroup | QButtonGroup * | src/widgets/shottoolwidget.cpp | 70 | ThicknessBtnGroup |  |  |
| 55 | m_scrollShotButton | ToolButton * | src/widgets/subtoolwidget.cpp | 142 | ScrollShotButton | ScrollShotButton | push button |
| 56 | m_ocrButton | ToolButton * | src/widgets/subtoolwidget.cpp | 146 | OcrButton | OcrButton | push button |
| 57 | m_ocrScrollButton | ToolButton * | src/widgets/subtoolwidget.cpp | 147 | OcrScrollButton | OcrScrollButton | push button |
| 58 | m_pinButton | ToolButton * | src/widgets/subtoolwidget.cpp | 151 | PinButton | PinButton | push button |
| 59 | m_cancelButton | ToolButton * | src/widgets/subtoolwidget.cpp | 152 | CancelButton | CancelButton | push button |
| 60 | m_recorderButton | ToolButton * | src/widgets/subtoolwidget.cpp | 153 | RecorderButton | RecorderButton | push button |
| 61 | m_gioButton | ToolButton * | src/widgets/subtoolwidget.cpp | 157 | GioButton | GioButton | push button |
| 62 | m_lineButton | ToolButton * | src/widgets/subtoolwidget.cpp | 161 | LineButton | LineButton | push button |
| 63 | m_arrowButton | ToolButton * | src/widgets/subtoolwidget.cpp | 165 | ArrowButton | ArrowButton | push button |
| 64 | m_penButton | ToolButton * | src/widgets/subtoolwidget.cpp | 169 | PenButton | PenButton | push button |
| 65 | m_mosaicButton | ToolButton * | src/widgets/subtoolwidget.cpp | 173 | MosaicButton | MosaicButton | push button |
| 66 | m_textButton | ToolButton * | src/widgets/subtoolwidget.cpp | 177 | TextButton | TextButton | push button |
| 67 | m_cameraButton | ToolButton * | src/widgets/subtoolwidget.cpp | 181 | CameraButton | CameraButton | push button |
| 68 | m_keyBoardButton | ToolButton * | src/widgets/subtoolwidget.cpp | 185 | KeyBoardButton | KeyBoardButton | push button |
| 69 | m_optionButton | ToolButton * | src/widgets/subtoolwidget.cpp | 189 | OptionButton | OptionButton | push button |
| 70 | m_scrollOptionButton | ToolButton * | src/widgets/subtoolwidget.cpp | 190 | ScrollOptionButton | ScrollOptionButton | push button |
| 71 | m_shotButton | ToolButton * | src/widgets/subtoolwidget.cpp | 191 | ShotButton | ShotButton | push button |
| 72 | m_shotOptionButton | ToolButton * | src/widgets/subtoolwidget.cpp | 195 | ShotOptionButton | ShotOptionButton | push button |
| 73 | m_aiAssistantButton | ToolButton * | src/widgets/subtoolwidget.cpp | 201 | AiAssistantButton | AiAssistantButton | push button |
| 74 | m_aiAssistantScrollButton | ToolButton * | src/widgets/subtoolwidget.cpp | 205 | AiAssistantScrollButton | AiAssistantScrollButton | push button |
| 75 | m_optionMenu | DMenu * | src/widgets/subtoolwidget.cpp | 209 | OptionMenu | OptionMenu | menu |
| 76 | m_scrollOptionMenu | DMenu * | src/widgets/subtoolwidget.cpp | 210 | ScrollOptionMenu | ScrollOptionMenu | menu |
| 77 | m_recordOptionMenu | DMenu * | src/widgets/subtoolwidget.cpp | 211 | RecordOptionMenu | RecordOptionMenu | menu |
| 78 | m_microphoneAction | QAction * | src/widgets/subtoolwidget.cpp | 214 | MicrophoneAction |  | push button |
| 79 | m_sysAudioAction | QAction * | src/widgets/subtoolwidget.cpp | 215 | SysAudioAction |  | push button |
| 80 | m_shotBtnGroup | QButtonGroup * | src/widgets/subtoolwidget.cpp | 216 | ShotBtnGroup |  |  |
| 81 | m_saveToSpecialPathMenu | DMenu * | src/widgets/subtoolwidget.cpp | 221 |  |  | menu |
| 82 | m_saveToSpecialPathAction | QAction * | src/widgets/subtoolwidget.cpp | 225 |  |  | push button |
| 83 | m_changeSaveToSpecialPath | QAction * | src/widgets/subtoolwidget.cpp | 229 |  |  | push button |
| 84 | m_scrollSaveToSpecialPathMenu | DMenu * | src/widgets/subtoolwidget.cpp | 234 |  |  | menu |
| 85 | m_scrollSaveToSpecialPathAction | QAction * | src/widgets/subtoolwidget.cpp | 238 |  |  | push button |
| 86 | m_scrollChangeSaveToSpecialPath | QAction * | src/widgets/subtoolwidget.cpp | 242 |  |  | push button |
| 87 | m_hSeparatorLine | DLabel * | src/widgets/toolbar.cpp | 82 | HorSeparatorLine |  | label |
| 88 | m_shotOptionButton | ToolButton * | src/widgets/toolbar.cpp | 91 |  |  | push button |
| 89 | m_closeButton | ToolButton * | src/widgets/toolbar.cpp | 96 | CloseButton | CloseButton | push button |
| 90 | m_confirmButton | ToolButton * | src/widgets/toolbar.cpp | 100 | ConfirmButton | ConfirmButton | push button |
| 91 | textLable | DLabel * | src/widgets/tooltips.cpp | 36 | TipText |  | label |

### C++ 缺失控件 (Gap)

无缺失
