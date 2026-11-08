<!--
 * @Author: wthrwe zxy_15208343098@qq.com
 * @Date: 2026-11-08 12:53:59
 * @LastEditors: wthrwe zxy_15208343098@qq.com
 * @LastEditTime: 2026-11-08 12:54:36
 * @FilePath: \undefinede:\FREERTOS\18-视觉跟踪二维打靶系统\Shooting_System\AVE\RWADME.md
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
-->
# 视觉跟踪二维打靶云台系统
基于STM32 + MaixCAM的嵌入式视觉跟踪打靶项目，实现目标识别、云台跟随、激光打靶功能。

## 项目简介
本项目采用MaixCAM运行YOLO目标检测，通过串口将目标坐标下发至STM32主控；STM32基于FreeRTOS做任务调度，使用单环P控制云台电机，实现目标自动跟踪，配套激光打靶。
项目重点解决串口帧同步、多任务调度、控制死区、工程配置易被CubeMX篡改等嵌入式工程常见问题，增加整机自检、日志调试能力。

## 硬件架构
- 主控：STM32F407
- 视觉模块：MaixCAM（YOLO目标检测）
- 执行机构：两路步进电机驱动二维云台
- 通信：USART串口
- 系统：FreeRTOS（多任务）

## 软件任务划分
1. VisionTask：串口接收坐标，控制闭环（最高优先级，20ms控制周期）
2. MotorTask：电机驱动输出
3. LogTask：异步日志输出、状态打印
4. SelfTestTask：开机自检（TIM寄存器、复位状态、轴校验）

## 难点与调试记录
详细故障复盘、排查思路、根因和解决方案：
👉 [调试日志](AVE/docs/debug-log.md)

## 主要功能
- YOLO视觉目标识别，串口输出目标坐标
- 二维云台跟随目标，带8px死区抑制抖动
- 转速限幅，抑制机械惯性带来的过冲震荡
- 单生产者单消费者环形缓冲区，解决串口帧撕裂
- 任务调度限流，避免高流量串口抢占CPU导致系统假死
- TIM寄存器开机自检，规避CubeMX重生成篡改定时器配置
- 整机自检脚本、编码检测工具，保障工程稳定性

## 工程目录说明

## 📝 Git 版本管理
本项目使用 Git 完成源码版本维护，做好工程文件管理：
- 配置 `.gitignore` 过滤 Keil MDK 编译产物、缓存文件，避免临时文件提交污染版本库；
- 采用规范提交注释：`feat`新增功能、`fix`修复bug、`docs`文档更新、`chore`工程整理；
- 使用 `main` 作为稳定主分支，开发调试在临时分支完成，验证通过后再合并回主分支，保证 main 分支代码可直接编译；
- 小步提交，每个功能、每处bug修复独立提交，保留完整提交历史，方便问题回溯与版本对比。

## 编译与运行
1. 使用Keil MDK打开 MDK‑ARM 工程
2. 编译下载到STM32
3. MaixCAM端部署YOLO检测脚本，上电自动开始识别
4. 串口工具可查看运行日志、自检信息

## 后续优化方向
- 增加增量式PID，替代纯P控制，提升跟踪稳定性
- 增加串口协议校验（CRC）
- 增加上位机QT/LVGL界面调试参数
