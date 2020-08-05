#ifndef CHIME_MEETINGS_VIEW_H
#define CHIME_MEETINGS_VIEW_H

#include <gtk/gtk.h>

#include "chime-contact.h"
#include "chime-conversation.h"
#include "chime-room.h"
#include "chime-meeting.h"

G_BEGIN_DECLS

#define CHIME_TYPE_MEETINGS_VIEW (chime_meetings_view_get_type ())
G_DECLARE_FINAL_TYPE(ChimeMeetingsView, chime_meetings_view, CHIME, MEETINGS_VIEW, GtkGrid)

GtkWidget *chime_meetings_view_new (void);

void       chime_meetings_view_set_connection      (ChimeMeetingsView *self,
                                                    ChimeConnection   *connection);

G_END_DECLS

#endif /* CHIME_MEETINGS_VIEW_H */

/* ex:set ts=4 et: */
