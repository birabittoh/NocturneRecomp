#pragma once

// Maps a track's filename to its pretty display title.

#include <string_view>
#include <unordered_map>

namespace nocturne {

// Returns the mapped title for a track filename key, or an empty view if
// no title is known for it.
inline std::string_view PrettyTrackName(std::string_view key) {
  static const std::unordered_map<std::string_view, std::string_view> kTrackNames = {
      {"artbgm", "The Lost Portrait"},
      {"bfboss", "Gate of Holy Spirits"},
      {"bfshite", "Cursed Sanctuary"},
      {"bgmusic", "Prayer"},
      {"church", "Requiem of the Gods"},
      {"dance", "Waltz of the Pearls"},
      {"dlboss", "Black Banquet"},
      {"douku", "Abandoned Pit"},
      {"end", "Transformation No.3"},
      {"gover", "Land of Benediction"},
      {"hall", "Golden Dance"},
      {"hekiru", "Nocturne"},
      {"iam", "I am the Wind"},
      {"katabgm", "Rainbow's Cemetery"},
      {"lboss", "The Poetic Ballad of Death"},
      {"libbgm", "Wood-Carved Partita"},
      {"mizu", "Crystal Teardrops"},
      {"mvroom", "Gates of Heaven"},
      {"name", "Prayer"},
      {"nares", "Symphony of the Night"},
      {"open", "Transformation No.1"},
      {"rboss", "Strange Bloodlines"},
      {"rouka", "Marble Gallery"},
      {"sakasa", "The Final Toccata"},
      {"save", "Silence"},
      {"sentou", "The Festival of Servants"},
      {"shiro", "Dracula's Castle"},
      {"stage0", "Dance of Illusions"},
      {"start", "Prologue"},
      {"tokei", "The Tragic Prince"},
      {"toubgm", "Tower of Evil Mist"},
      {"tougi", "Wandering Ghosts"},
      {"turoom1", "Gates of Hell"},
      {"tyukan", "Transformation No.2"},
      {"wboss", "Demonic Banquet"},
      {"lament", "Admiration of a Clan"},
      // {"wind1", ""},
      // {"wind_ini", ""},
  };
  auto it = kTrackNames.find(key);
  return it != kTrackNames.end() ? it->second : std::string_view{};
}

}  // namespace nocturne
