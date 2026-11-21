# HD28091 屏幕与触摸转接板 Rev A

嘉立创EDA工程“哈基米机”中新增板子 `LCD-HD28091-Adapter`。显示屏为 HD28091C40-V1，可搭配微雪 ESP32-P4-WIFI6 或 ESP32-P4-Pico。触摸原图标注 CST328，当前实测模组在 `0x58` 使用 CST3530 协议。LCD和触摸分别采用独立RC上电复位。原理图使用A2原生图框及标题栏，按显示接口、屏幕供电、背光、触摸与复位四部分整理，电源电路内部使用连续导线。原理图说明及PCB文字丝印均使用中文；器件型号、网络名、引脚标识保留原技术标识。

[屏幕淘宝购买链接](https://item.taobao.com/item.htm?id=566355265504&mi_id=0000sGhHLYPMQHlue9llXrv9IkNEE6YnqL7G8TRNHJ512as&spm=tbpc.boughtlist.suborder_itemtitle.1.70d82e8dXaiCDY) · [转接板在线工程](https://oshwhub.com/fosen/project_tzekpmhv)

## 文件

- `HD28091-Schematic-RevA.epro2`：嘉立创EDA原生原理图，包含所用库资源。
- `HD28091-Adapter-RevA.epro2`：嘉立创EDA原生PCB，包含封装资源。
- `schematic.png`、`pcb.png`：原理图和PCB预览。
- `schematic.esch`、`pcb.epcb`：便于版本管理的文档源数据。
- `BOM.csv`：19个器件的物料表，包含立创料号。
- `netlist.json`、`verification.json`：导出的网表和检查记录。
- `layout-review.json`：按封装外形与焊盘范围检查的器件间隙、位置和分区。
- `impedance-estimate.json`、`tools/estimate_impedance.py`：双层差分主干的名义截面估算和计算脚本。

## 接口与供电

| 位号 | 用途 | 连接器 |
| --- | --- | --- |
| J1 | 微雪开发板DSI口，22Pin、0.5mm | AFC07-S22FCC-00，PCB顶面 |
| J2 | LCD排线，40Pin、0.5mm | FH12-40S-0.5SH(55)，PCB顶面 |
| J3 | 触摸排线，6Pin、0.5mm | AFC07-S06FCC-00，PCB顶面 |

J3现在是触摸接口。设计中没有额外的供电、复位或调光接线口。

LCD插座J2与触摸插座J3位于顶面，排线口均朝PCB坐标的+Y方向。两种封装的初始方向不同，因此J2旋转角为0°，J3为180°；不能根据旋转角相同判断出线方向。J1已改为顶面安装，中心坐标为(1180, 390)mil，旋转0°，插入口朝-Y方向，与LCD和触摸口相反；J2、J3位置和方向保留。背光驱动整组已移到排线路径左侧。J1前方至板下边缘预留约12.7mm宽的无器件通道（PCB坐标x=930–1430mil），并建立只禁止放置器件的约束区；走线、覆铜和齐平过孔可通过。禁放区的原生规则已通过保存重开核验。

J1.1接开发板3.3V，经ME6211C30M5G-N生成LCD的3.0V。SY7200AABC也从该3.3V取电，升压驱动背光；R1为5.6Ω、1%，标称电流约35.7mA。背光EN接输入电源，固定开启。CST328使用3.3V电源和开发板现有I²C总线；主板已有I²C上拉，本板没有重复增加上拉。

| 信号 | J1主控端 | J2显示端 |
| --- | --- | --- |
| DSI CLK P / N | 14 / 15 | 9 / 8 |
| DSI D0 P / N | 20 / 21 | 6 / 5 |
| DSI D1 P / N | 17 / 18 | 12 / 11 |
| LCD 3.0V | 板上稳压生成 | 3、38、40 |
| LEDA / LEDK | 板上恒流升压 | 1 / 2 |
| LCD RESET | R2、C6、D2生成 | 34 |

J3引脚依次为：1 GND、2 RESET、3 INT、4 SDA、5 SCL、6 VDD。SDA接J1.2，SCL接J1.3，分别对应微雪主板GPIO7和GPIO8。INT引到单独测试焊盘，默认悬空。当前配套固件将`BSP_LCD_TOUCH_INT`设为`GPIO_NUM_NC`，每15ms通过I²C轮询触摸。若将该焊盘飞线接GPIO22，可在所选PlatformIO环境的`build_flags`中追加`-DBSP_LCD_TOUCH_INT=22`，启用下降沿中断唤醒。

LCD复位使用R2=10kΩ、C6=1µF，电阻接3.0V，电容接地，中点为`LCD_RST`。触摸复位使用R3=10kΩ、C8=1µF，电阻接3.3V，电容接地，中点为`TP_RST`，接J3.2。两路互相独立。

D2、D3均为BAT54WS，用于掉电时快速放电：阳极接各自RESET，阴极接各自电源。每路RC时间常数约10ms，这是样机起始值，并不保证10ms的有效复位脉宽；实际时序取决于电源上升速度、输入门限及元件误差，需要实测确认。设计中已移除复位IC，不再具有原复位芯片的欠压检测和定时释放功能。

主控排线必须满足J1与开发板接口的同编号引脚导通，即1→1、2→2。采购时应根据两端插座接触面判断排线形式，不能只根据“同向/反向”的商品名称判断。

## PCB与检查

板框为40.64 × 38.10mm，双层，采用EDA保存后的常规1.6mm叠层，两面铜厚约35µm。顶层布置器件和主要走线；两面均采用GND实心覆铜，关闭孤岛保留，并用地过孔连接。SY7200的输入电容、采样电阻和输出电容保留必要的短地回流；细间距接口处保留覆铜无法独立完成连接的短地线。全局覆铜焊盘规则保持原有热焊盘设置。

器件按屏幕供电、LCD复位、触摸与复位、背光四部分集中布置。C1/U1/C2位于LCD接口下方；两套RC分别靠近对应接口；C3、U2、L1、D1、C4、R1集中在背光区。检查同时包含焊盘及库中器件外形的包围范围，本次检查确认排线通道内无器件；背光区移位后，与相邻器件的封装几何范围间隙最小约0.53mm。该检查不代替FPC实物插接及整机装配核验。

J1朝板下方出线，LCD和触摸朝上方。本次将SY7200、L1、D1、C3、C4、R1集中移到通道左侧，输入电容、开关节点、采样反馈和地回流按功能就近连接，重接3.3V供电及背光输出。按用户调整后的J1位置重接主控侧线路，以既有屏侧主线为基础重接主控侧信号、重新匹配长度，移除衔接处环回，并重布受影响的I²C、3.3V供电与背光输出。全部器件的位置和朝向与用户本次调整后的状态一致。差分对内长度差检查通过，当前最大约3.2mil；具体长度、线宽和过孔数量见`verification.json`。

此前16mil线宽、14mil间距均匀截面的阻抗估算已标记为历史记录，不适用于当前全部布线。打样前仍需由板厂按实际叠层及当前线宽、间距、覆铜和扇出条件复核100Ω目标阻抗。

2026-11-03已核对19个器件的113个引脚映射，其中106个已连接、7个NC；原理图与PCB的引脚网络及封装一致。双层板及两面覆铜的最终PCB DRC为0项，保存并重新打开后再次通过检查，实际启用的铜层仅为顶层和底层。原理图图框和中文排版保留。原理图ERC没有error，保留INT单引脚网络提示和部分器件显示值与库属性不同提示；制造商型号和物料号保留在BOM中。

## 固件和实物验证

上电后等待至少1秒，再初始化显示和触摸。复位由两套RC产生，固件将硬复位引脚设为NC，LCD软件复位后等待120ms。当前分支使用屏厂提供的BOE初始化表，CST328和CST3530默认采用15ms轮询读取。CST328 可选 GPIO22 中断。寄存器、DSI时序和板型构建目标见[硬件改动与固件适配说明](../../docs/hd28091-hardware-adaptation.md)。

Pico v3.2 和 WiFi6 v3.2 均已在 R292 适配固件上验证显示初始化和 CST3530 轮询。拔插 J3 对照确认 `0x58` 来自触摸模组。CST3530 坐标修正后的实体点击位置待复测。LCD负载下的3.0V电源、背光电流、两路RESET波形和背光均匀性待测。RC复位测试需覆盖正常上电、慢上电、短暂掉电和快速断电重启。两根屏侧排线的位置参考用户提供的`HD28091C40-V1总成(2).jpg`，外形装配仍需实物复核。

## 资料

- 用户提供的`HD28091C40-V1规格书(2).pdf`、独立触摸图和最终屏幕触摸总成图。
- [微雪ESP32-P4-WIFI6硬件资料](https://docs.waveshare.net/ESP32-P4-WIFI6/Resources-And-Documents/)。
- [SY7200A官方规格书](https://www.silergy.com/zh/download/downloadFile?ftype=note&id=4351&type=product)。
- [乐鑫ESP32-P4 PCB布局指南](https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32p4/pcb-layout-design-esp32p4.html)。
- [微雪CST328驱动](https://github.com/waveshareteam/Waveshare-ESP32-components/blob/master/display/touch/esp_lcd_touch_cst328/esp_lcd_touch_cst328.c)。
