#pragma once

// 页面条目 -> TSL 配置块位域（byte / shift / mask）。
// bias  = 页面取值 + bias 才是模块取值（轻鸣笛次数页面从 1 开始）
// maxOut= 模块接受的取值上限，超过即拒绝，不写坏相邻位
// 由 data/feifan-menu-data.json 的 fields_full 与条目实现注解交叉校验生成，共 44 条。
static const TslBit TSL_MAP[] = {
    {"d4a", 24, 0, 7, 0, 7},  // 自动运动模式 | autoSportMode
    {"d4b", 24, 5, 7, 0, 7},  // 能量回收制动 | KERS
    {"d4c", 25, 5, 3, 0, 3},  // 停止模式 | stoppingMode
    {"d4d", 24, 4, 1, 0, 1},  // 自动强驻车 | autoParkBrake
    {"l1a", 25, 0, 3, 0, 3},  // 远光爆闪 | pluseHighBeam
    {"l1b", 25, 2, 1, 0, 1},  // 近光跟随远光爆闪 | pluseHighBeamAndLowBeam
    {"l1c", 30, 6, 1, 0, 1},  // 远光闪 3 次 | blinkHighBeam
    {"l2a", 27, 6, 3, 0, 3},  // 脉冲喇叭 | pluseHorn
    {"l2b", 27, 5, 1, 0, 1},  // 长按喇叭开启脉冲 | longPluseHorn
    {"l2c", 34, 4, 3, 0, 3},  // 低速轻鸣笛 | gentryHorn
    {"l2d", 34, 6, 3, -1, 3},  // 轻鸣笛次数 | gentryHornCount
    {"l2e", 32, 5, 1, 0, 1},  // 喇叭联动远光 | hornLinkedHighBeam
    {"l2f", 32, 6, 1, 0, 1},  // 远光联动喇叭 | highBeamLinkedHorn
    {"l3a", 30, 5, 1, 0, 1},  // 前雾灯爆闪 | pluseFrontFogLight
    {"l3b", 31, 6, 1, 0, 1},  // 后雾灯爆闪 | pluseRearFogLight
    {"l3c", 33, 6, 3, 0, 2},  // 动态刹车灯 | dynamicBrakeLight
    {"l3d", 30, 4, 1, 0, 1},  // 开门双闪 | openDoorHazardLight
    {"l3e", 30, 7, 1, 0, 1},  // 倒车双闪 | backHazardLight
    {"l3f", 24, 3, 1, 0, 1},  // 倒车后雾灯照明 | backHazardRearFogLight
    {"l3g", 31, 7, 1, 0, 1},  // 转向辅助照明 | steerAssistanceLighting
    {"b2g", 25, 3, 3, 0, 3},  // 副驾便利进出 | frontRightEasyAccess
    {"b3a", 33, 4, 3, 0, 3},  // 空调烘干 | airAutoArid
    {"a1e", 29, 0, 3, 0, 3},  // 方向盘抑制方式 | ModeMDR
    {"a1f", 28, 0, 1, 0, 1},  // 时间随机 | rand
    {"a1g", 27, 0, 15, 0, 15},  // 滚轮操作时间 | time
    {"a1h", 32, 0, 15, 0, 15},  // 加减时间间隔 | jg
    {"a1i", 33, 0, 3, 0, 3},  // 加减类型 | jj
    {"a1j", 28, 6, 3, 0, 3},  // 滚轮选择 | lr
    {"a1k", 28, 4, 1, 0, 1},  // 自动免打扰 | automdr
    {"a2e", 31, 3, 1, 0, 1},  // 一键 AP | oneClickOpenAP
    {"a2f", 28, 1, 3, 0, 3},  // AP 自动恢复 | apAutoOpenType
    {"a2g", 30, 1, 7, 0, 7},  // AP 恢复延时 | apAutoOpenTime
    {"a2h", 31, 4, 3, 0, 3},  // 开启手动远光 | apHighBeam
    {"a2i", 34, 0, 1, 0, 1},  // AP 超速退出 | apOverspeedExit
    {"a2j", 25, 7, 1, 0, 1},  // 关闭自动雨刮 | OffAPAutoWiper
    {"a3a", 28, 5, 1, 0, 1},  // EAP 增强 | fsd
    {"b1e", 29, 2, 7, 0, 7},  // 门把手开关前备箱 | TimeOpenFrontBox
    {"b1f", 29, 5, 7, 0, 7},  // 门把手开关充电口 | TimeOpenChargeDoor
    {"b5a", 34, 1, 1, 0, 1},  // 主驾安全带提醒 | frontLeftBuckle
    {"b5b", 34, 2, 1, 0, 1},  // 副驾安全带提醒 | frontRightBuckle
    {"b5c", 34, 3, 1, 0, 1},  // 后排安全带提醒 | rearBuckle
    {"b5d", 26, 0, 255, 0, 255},  // 提示音音量 | beep
    {"b5e", 27, 4, 1, 0, 1},  // LED 灯开关 | led
    {"s3b", 28, 3, 1, 0, 1},  // 模块总开关 | mianSwitch
};
