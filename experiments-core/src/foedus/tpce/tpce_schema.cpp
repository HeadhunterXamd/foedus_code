/*
 * Copyright (c) 2014-2015, Hewlett-Packard Development Company, LP.
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details. You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 * HP designates this particular file as subject to the "Classpath" exception
 * as provided by HP in the LICENSE.txt file that accompanied this code.
 */
#include "foedus/tpce/tpce_schema.hpp"

#include <cstring>

#include "foedus/engine.hpp"
#include "foedus/storage/storage.hpp"
#include "foedus/storage/storage_manager.hpp"

namespace foedus {
namespace tpce {

TpceStorages::TpceStorages() {
  std::memset(static_cast<void*>(this), 0, sizeof(TpceStorages));
}
void TpceStorages::assert_initialized() {
  ASSERT_ND(trades_.exists());
  ASSERT_ND(trades_secondary_symb_dts_.exists());
  ASSERT_ND(trade_types_.exists());

  ASSERT_ND(trades_.get_name().str() == "trades");
  ASSERT_ND(trades_secondary_symb_dts_.get_name().str() == "trades_secondary_symb_dts");
  ASSERT_ND(trade_types_.get_name().str() == "trade_types");
}

void TpceStorages::initialize_tables(Engine* engine) {
  storage::StorageManager* st = engine->get_storage_manager();
  trades_ = st->get_hash("trades");
  trades_secondary_symb_dts_ = st->get_masstree("trades_secondary_symb_dts");
  trade_types_ = st->get_array("trade_types");
  assert_initialized();
}

}  // namespace tpce
}  // namespace foedus
