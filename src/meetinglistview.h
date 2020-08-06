#ifndef CHIME_MEETING_LIST_VIEW_H
#define CHIME_MEETING_LIST_VIEW_H

#include <gtk/gtk.h>

#include "chime-contact.h"
#include "chime-conversation.h"
#include "chime-room.h"
#include "chime-meeting.h"

G_BEGIN_DECLS

#define CHIME_TYPE_MEETING_LIST_VIEW (chime_meeting_list_view_get_type ())
G_DECLARE_FINAL_TYPE(ChimeMeetingListView, chime_meeting_list_view, CHIME, MEETING_LIST_VIEW, GtkGrid)

GtkWidget *chime_meeting_list_view_new (void);

void       chime_meeting_list_view_set_connection      (ChimeMeetingListView *self,
                                                        ChimeConnection      *connection);

G_END_DECLS

#endif /* CHIME_MEETING_LIST_VIEW_H */

/* ex:set ts=4 et: */
