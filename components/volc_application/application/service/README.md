# Service

## brief introduction

This directory presents the business module division of the Hardware Conversational Agent Development Kit for AI conversation services. All business modules are named "service", and different services communicate through event subscription (sub) and publication (pub) at the framework layer. The main modules are as follows:

- conv_service：The core module of the AI Conversational Agent, responsible for the initiation, operation, and termination of AI conversation services.
- function_call_service：Interacts with the conv\_service module, responsible for executing function calls and returning the results to the agent.
- local_logic_service：A local module based on predefined business logic (not the agent's business logic). It performs tasks such as parsing subtitles and taking certain actions based on quick responses to the subtitles (different from function calls).
- src: An externally exposed module responsible for initiating the AI conversation.
