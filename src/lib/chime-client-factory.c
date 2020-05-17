/* chime-client-factory.c
 *
 * Copyright © 2017 Florian Müllner <fmuellner@gnome.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "chime-client-factory.h"

G_DEFINE_TYPE (ChimeClientFactory, chime_client_factory, TP_TYPE_AUTOMATIC_CLIENT_FACTORY)

ChimeClientFactory *
chime_client_factory_new (void)
{
  return g_object_new (CHIME_TYPE_CLIENT_FACTORY, NULL);
}

/**
 * chime_client_factory_create_account:
 * Returns: (transfer full):
 */
TpAccount *
chime_client_factory_create_account (ChimeClientFactory  *self,
                                      const char           *object_path,
                                      GError              **error)
{
  ChimeClientFactoryClass *klass = CHIME_CLIENT_FACTORY_GET_CLASS (self);
  TpSimpleClientFactoryClass *simple_class =
    TP_SIMPLE_CLIENT_FACTORY_CLASS (chime_client_factory_parent_class);

  if (klass->create_account)
    return klass->create_account (self, object_path, error);

  return simple_class->create_account (TP_SIMPLE_CLIENT_FACTORY (self),
                                       object_path,
                                       NULL,
                                       error);
}

static TpAccount *
chime_client_factory_create_account_impl (TpSimpleClientFactory  *self,
                                           const char             *object_path,
                                           const GHashTable       *immutable_props,
                                           GError                **error)
{
  return chime_client_factory_create_account (CHIME_CLIENT_FACTORY (self),
                                               object_path,
                                               error);
}

static void
chime_client_factory_class_init (ChimeClientFactoryClass *klass)
{
  TpSimpleClientFactoryClass *simple_class = TP_SIMPLE_CLIENT_FACTORY_CLASS (klass);

  simple_class->create_account = chime_client_factory_create_account_impl;
}

static void
chime_client_factory_init (ChimeClientFactory *self)
{
}
