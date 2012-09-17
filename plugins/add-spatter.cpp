#include "Core.h"
#include <Console.h>
#include <Export.h>
#include <PluginManager.h>
#include <modules/Gui.h>
#include <modules/Screen.h>
#include <modules/Maps.h>
#include <modules/Job.h>
#include <modules/Items.h>
#include <TileTypes.h>
#include <vector>
#include <cstdio>
#include <stack>
#include <string>
#include <cmath>
#include <string.h>

#include <VTableInterpose.h>
#include "df/item_liquid_miscst.h"
#include "df/item_constructed.h"
#include "df/builtin_mats.h"
#include "df/world.h"
#include "df/job.h"
#include "df/job_item.h"
#include "df/job_item_ref.h"
#include "df/ui.h"
#include "df/report.h"
#include "df/reaction.h"
#include "df/reaction_reagent_itemst.h"
#include "df/reaction_product_item_improvementst.h"
#include "df/reaction_product_improvement_flags.h"
#include "df/matter_state.h"
#include "df/contaminant.h"

#include "MiscUtils.h"

using std::vector;
using std::string;
using std::stack;
using namespace DFHack;
using namespace df::enums;

using df::global::gps;
using df::global::world;
using df::global::ui;

typedef df::reaction_product_item_improvementst improvement_product;

DFHACK_PLUGIN("add-spatter");

struct ReagentSource {
    int idx;
    df::reaction_reagent *reagent;

    ReagentSource() : idx(-1), reagent(NULL) {}
};

struct MaterialSource : ReagentSource {
    bool product;
    std::string product_name;

    int mat_type, mat_index;

    MaterialSource() : product(false), mat_type(-1), mat_index(-1) {}
};

struct ProductInfo {
    df::reaction *react;
    improvement_product *product;

    ReagentSource object;
    MaterialSource material;

    bool isValid() {
        return object.reagent && (material.mat_type >= 0 || material.reagent);
    }
};

struct ReactionInfo {
    df::reaction *react;

    std::vector<ProductInfo> products;
};

static std::map<std::string, ReactionInfo> reactions;
static std::map<df::reaction_product*, ProductInfo*> products;

static ReactionInfo *find_reaction(const std::string &name)
{
    auto it = reactions.find(name);
    return (it != reactions.end()) ? &it->second : NULL;
}

static bool is_add_spatter(const std::string &name)
{
    return name.size() > 12 && memcmp(name.data(), "SPATTER_ADD_", 12) == 0;
}

static void find_material(int *type, int *index, df::item *input, MaterialSource &mat)
{
    if (input && mat.reagent)
    {
        MaterialInfo info(input);

        if (mat.product)
        {
            if (!info.findProduct(info, mat.product_name))
            {
                color_ostream_proxy out(Core::getInstance().getConsole());
                out.printerr("Cannot find product '%s'\n", mat.product_name.c_str());
            }
        }

        *type = info.type;
        *index = info.index;
    }
    else
    {
        *type = mat.mat_type;
        *index = mat.mat_index;
    }
}

static bool has_contaminant(df::item_actual *item, int type, int index)
{
    auto cont = item->contaminants;
    if (!cont)
        return false;

    for (size_t i = 0; i < cont->size(); i++)
    {
        auto cur = (*cont)[i];
        if (cur->mat_type == type && cur->mat_index == index)
            return true;
    }

    return false;
}

/*
 * Hooks
 */

typedef std::map<int, std::vector<df::item*> > item_table;

static void index_items(item_table &table, df::job *job, ReactionInfo *info)
{
    for (int i = job->items.size()-1; i >= 0; i--)
    {
        auto iref = job->items[i];
        if (iref->job_item_idx < 0) continue;
        auto iitem = job->job_items[iref->job_item_idx];

        if (iitem->contains.empty())
        {
            table[iitem->reagent_index].push_back(iref->item);
        }
        else
        {
            std::vector<df::item*> contents;
            Items::getContainedItems(iref->item, &contents);

            for (int j = contents.size()-1; j >= 0; j--)
            {
                for (int k = iitem->contains.size()-1; k >= 0; k--)
                {
                    int ridx = iitem->contains[k];
                    auto reag = info->react->reagents[ridx];

                    if (reag->matches(contents[j], info->react, iitem->reaction_id))
                        table[ridx].push_back(contents[j]);
                }
            }
        }
    }
}

df::item* find_item(ReagentSource &info, item_table &table)
{
    if (!info.reagent)
        return NULL;
    if (table[info.idx].empty())
        return NULL;
    return table[info.idx].back();
}

struct item_hook : df::item_constructed {
    typedef df::item_constructed interpose_base;

    DEFINE_VMETHOD_INTERPOSE(bool, isImprovable, (df::job *job, int16_t mat_type, int32_t mat_index))
    {
        ReactionInfo *info;

        if (job && job->job_type == job_type::CustomReaction &&
            (info = find_reaction(job->reaction_name)) != NULL)
        {
            if (!contaminants || contaminants->empty())
                return true;

            item_table table;
            index_items(table, job, info);

            for (size_t i = 0; i < info->products.size(); i++)
            {
                auto &product = info->products[i];

                int mattype, matindex;
                auto material = find_item(info->products[i].material, table);

                find_material(&mattype, &matindex, material, product.material);

                if (mattype < 0 || has_contaminant(this, mattype, matindex))
                    return false;
            }

            return true;
        }

        return INTERPOSE_NEXT(isImprovable)(job, mat_type, mat_index);
    }
};

IMPLEMENT_VMETHOD_INTERPOSE(item_hook, isImprovable);

df::item* find_item(
    ReagentSource &info,
    std::vector<df::reaction_reagent*> *in_reag,
    std::vector<df::item*> *in_items
) {
    if (!info.reagent)
        return NULL;
    for (int i = in_items->size(); i >= 0; i--)
        if ((*in_reag)[i] == info.reagent)
            return (*in_items)[i];
    return NULL;
}

struct product_hook : improvement_product {
    typedef improvement_product interpose_base;

    DEFINE_VMETHOD_INTERPOSE(
        void, produce,
        (df::unit *unit, std::vector<df::item*> *out_items,
         std::vector<df::reaction_reagent*> *in_reag,
         std::vector<df::item*> *in_items,
         int32_t quantity, int16_t skill,
         df::historical_entity *entity, df::world_site *site)
    ) {
        if (auto product = products[this])
        {
            auto object = find_item(product->object, in_reag, in_items);
            auto material = find_item(product->material, in_reag, in_items);

            if (object && (material || !product->material.reagent))
            {
                int mattype, matindex;
                find_material(&mattype, &matindex, material, product->material);

                object->addContaminant(
                    mattype, matindex,
                    matter_state::Liquid, // TODO: heuristics or by reagent name
                    object->getTemperature(),
                    probability, // used as size
                    -1,
                    0x8000 // not washed by water, and 'clean items' safe.
                );
            }

            return;
        }

        INTERPOSE_NEXT(produce)(unit, out_items, in_reag, in_items, quantity, skill, entity, site);
    }
};

IMPLEMENT_VMETHOD_INTERPOSE(product_hook, produce);

/*
 * Scan raws for matching reactions.
 */

static void find_reagent(
    color_ostream &out, ReagentSource &info, df::reaction *react, std::string name
) {
    for (size_t i = 0; i < react->reagents.size(); i++)
    {
        if (react->reagents[i]->code != name)
            continue;

        info.idx = i;
        info.reagent = react->reagents[i];
        return;
    }

    out.printerr("Invalid reagent name '%s' in '%s'\n", name.c_str(), react->code.c_str());
}

static void parse_product(
    color_ostream &out, ProductInfo &info, df::reaction *react, improvement_product *prod
) {
    using namespace df::enums::reaction_product_improvement_flags;

    info.react = react;
    info.product = prod;

    find_reagent(out, info.object, react, prod->target_reagent);

    auto ritem = strict_virtual_cast<df::reaction_reagent_itemst>(info.object.reagent);
    if (ritem)
        ritem->flags1.bits.improvable = true;

    info.material.mat_type = prod->mat_type;
    info.material.mat_index = prod->mat_index;

    if (prod->flags.is_set(GET_MATERIAL_PRODUCT))
    {
        find_reagent(out, info.material, react, prod->get_material.reagent_code);

        info.material.product = true;
        info.material.product_name = prod->get_material.product_code;
    }
    else if (prod->flags.is_set(GET_MATERIAL_SAME))
    {
        find_reagent(out, info.material, react, prod->get_material.reagent_code);
    }
}

static bool find_reactions(color_ostream &out)
{
    reactions.clear();
    products.clear();

    auto &rlist = world->raws.reactions;

    for (size_t i = 0; i < rlist.size(); i++)
    {
        if (!is_add_spatter(rlist[i]->code))
            continue;

        reactions[rlist[i]->code].react = rlist[i];
    }

    for (auto it = reactions.begin(); it != reactions.end(); ++it)
    {
        auto &prod = it->second.react->products;
        auto &out_prod = it->second.products;

        for (size_t i = 0; i < prod.size(); i++)
        {
            auto itprod = strict_virtual_cast<improvement_product>(prod[i]);
            if (!itprod) continue;

            out_prod.push_back(ProductInfo());
            parse_product(out, out_prod.back(), it->second.react, itprod);
        }

        for (size_t i = 0; i < prod.size(); i++)
        {
            if (out_prod[i].isValid())
                products[out_prod[i].product] = &out_prod[i];
        }
    }

    return !products.empty();
}

static void enable_hooks(bool enable)
{
    INTERPOSE_HOOK(item_hook, isImprovable).apply(enable);
    INTERPOSE_HOOK(product_hook, produce).apply(enable);
}

DFhackCExport command_result plugin_onstatechange(color_ostream &out, state_change_event event)
{
    switch (event) {
    case SC_MAP_LOADED:
        if (find_reactions(out))
        {
            out.print("Detected spatter add reactions - enabling plugin.\n");
            enable_hooks(true);
        }
        else
            enable_hooks(false);
        break;
    case SC_MAP_UNLOADED:
        enable_hooks(false);
        reactions.clear();
        products.clear();
        break;
    default:
        break;
    }

    return CR_OK;
}

DFhackCExport command_result plugin_init ( color_ostream &out, std::vector <PluginCommand> &commands)
{
    if (Core::getInstance().isMapLoaded())
        plugin_onstatechange(out, SC_MAP_LOADED);

    return CR_OK;
}

DFhackCExport command_result plugin_shutdown ( color_ostream &out )
{
    enable_hooks(false);
    return CR_OK;
}
