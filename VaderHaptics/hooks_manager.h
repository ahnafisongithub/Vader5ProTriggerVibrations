#ifndef HOOKS_MANAGER_H
#define HOOKS_MANAGER_H

#ifdef __cplusplus
extern "C" {
#endif

void Hooks_Install(void);
void Hooks_PollLateModules(void);
void Hooks_Uninstall(void);
int  Hooks_DetectGameMotorCount(void);

#ifdef __cplusplus
}
#endif

#endif // HOOKS_MANAGER_H
