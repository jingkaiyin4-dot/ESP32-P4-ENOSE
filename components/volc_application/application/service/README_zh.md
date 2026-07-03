# Service

## 目录简介

本目录是硬件对话智能体开发套件对于ai对话业务进行的业务模块划分，不同的业务模块我们以service命名；不同的service通过framework层event的sub和pub进行通信。主要分为一下模块

- conv_service： 智能体ai对话的主要模块，负责ai对话业务的开始，运行，与结束。
- function_call_service： 与conv_service模块交互，负责function call的执行并将结果返回给智能体
- local_logic_service： 本地基于既定业务逻辑（非智能体的业务逻辑）的模块，例如解析字幕并根据字幕快速响应做出某些action等（区别于function call）
- src: 对外暴露的模块，负责ai对话的拉起

