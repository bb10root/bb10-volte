#ifndef VOICE_FLAG_H
#define VOICE_FLAG_H

extern int g_force_ims_voice;

unsigned long next_network_service(unsigned long service, int ims_up);
void sync_voice_flag(int allowed);

#endif /* VOICE_FLAG_H */
