#include "MapGridManager.h"
#include "GridObjectLoader.h"
#include "GridTerrainLoader.h"

void MapGridManager::CreateGrid(uint16 const x, uint16 const y)
{
    std::lock_guard<std::mutex> guard(_gridLock);
    if (IsGridCreatedUnlocked(x, y))
        return;

    // If we are an instance, ensure parent map has already created the grid before proceeding.
    // Note: grid loading is locked and is safe across multiple map threads in the
    // event of multiple child maps attempting to create the same parent map grid
    if (_map->GetInstanceId() != 0)
    {
        Map* parentMap = const_cast<Map*>(_map->GetParent());
        parentMap->EnsureGridCreated(GridCoord(x, y));
    }

    std::unique_ptr<MapGridType> grid = std::make_unique<MapGridType>(x, y);
    grid->link(_map);

    // Terrain is loading during create (should/can we move this to LoadGrid?)
    // This takes the shared vmap tree and navmesh write locks while _gridLock is held.
    GridTerrainLoader loader(*grid, _map);
    loader.LoadTerrain();

    _mapGrid[x][y] = std::move(grid);

    ++_createdGridsCount;
}

// Loads objects, terrain already loaded in CreateGrid
bool MapGridManager::LoadGrid(uint16 const x, uint16 const y)
{
    // Seal task or between beats; the flag makes a re-entry from the spawning below a no-op.
    MapGridType* grid = GetGridUnlocked(x, y);
    if (!grid || grid->IsObjectDataLoaded())
        return false;

    // Must mark as loaded first, as GridObjectLoader spawning objects can attempt to recursively load the grid
    grid->SetObjectDataLoaded();
    ++_loadedGridsCount;

    GridObjectLoader loader(*grid, _map);
    loader.LoadAllCellsInGrid();

    return true;
}

void MapGridManager::UnloadGrid(uint16 const x, uint16 const y)
{
    MapGridType* grid = GetGrid(x, y);
    if (!grid)
        return;

    {
        GridObjectCleaner worker;
        TypeContainerVisitor<GridObjectCleaner, GridTypeMapContainer> visitor(worker);
        grid->VisitAllCells(visitor);
    }

    _map->RemoveAllObjectsInRemoveList();

    {
        GridObjectUnloader worker;
        TypeContainerVisitor<GridObjectUnloader, GridTypeMapContainer> visitor(worker);
        grid->VisitAllCells(visitor);
    }

    // Seal task or between beats: no reader is running (step 14).
    _mapGrid[x][y] = nullptr;
}

bool MapGridManager::IsGridCreatedUnlocked(uint16 const x, uint16 const y) const
{
    if (!MapGridManager::IsValidGridCoordinates(x, y))
        return false;

    return _mapGrid[x][y].get();
}

bool MapGridManager::IsGridLoadedUnlocked(uint16 const x, uint16 const y) const
{
    if (!MapGridManager::IsValidGridCoordinates(x, y))
        return false;

    return _mapGrid[x][y].get() && _mapGrid[x][y]->IsObjectDataLoaded();
}

MapGridType* MapGridManager::GetGridUnlocked(uint16 const x, uint16 const y) const
{
    if (!MapGridManager::IsValidGridCoordinates(x, y))
        return nullptr;

    return _mapGrid[x][y].get();
}

// Step 14: the readers take nothing. The grid array mutates in CreateGrid / UnloadGrid only, both of which run
// in a seal task or between beats, when no unit task reads; a lock here would be one line bouncing between
// every core that visits a cell.
bool MapGridManager::IsGridCreated(uint16 const x, uint16 const y) const
{
    return IsGridCreatedUnlocked(x, y);
}

bool MapGridManager::IsGridLoaded(uint16 const x, uint16 const y) const
{
    return IsGridLoadedUnlocked(x, y);
}

MapGridType* MapGridManager::GetGrid(uint16 const x, uint16 const y) const
{
    return GetGridUnlocked(x, y);
}

uint32 MapGridManager::GetCreatedGridsCount()
{
    return _createdGridsCount;
}

uint32 MapGridManager::GetLoadedGridsCount()
{
    return _loadedGridsCount;
}

uint32 MapGridManager::GetCreatedCellsInGridCount(uint16 const x, uint16 const y)
{
    MapGridType* grid = GetGrid(x, y);
    if (grid)
        return grid->GetCreatedCellsCount();

    return 0;
}

uint32 MapGridManager::GetCreatedCellsInMapCount()
{
    uint32 count = 0;
    for (uint32 gridX = 0; gridX < MAX_NUMBER_OF_GRIDS; ++gridX)
    {
        for (uint32 gridY = 0; gridY < MAX_NUMBER_OF_GRIDS; ++gridY)
        {
            if (MapGridType* grid = GetGrid(gridX, gridY))
                count += grid->GetCreatedCellsCount();
        }
    }
    return count;
}

bool MapGridManager::IsGridsFullyCreated() const
{
    return _createdGridsCount == (MAX_NUMBER_OF_GRIDS * MAX_NUMBER_OF_GRIDS);
}

bool MapGridManager::IsGridsFullyLoaded() const
{
    return _loadedGridsCount == (MAX_NUMBER_OF_GRIDS * MAX_NUMBER_OF_GRIDS);
}
