// Copyright (C) 2021 Ikomia SAS
// Contact: https://www.ikomia.com
//
// This file is part of the IkomiaStudio software.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include "CWorkflowRunManager.h"
#include <QtConcurrent/QtConcurrent>
#include "Main/LogCategory.h"
#include "Model/Project/CProjectManager.h"
#include "Model/Data/CMainDataManager.h"
#include "Model/ProgressBar/CProgressBarManager.h"
#include "IO/CPathIO.h"

CWorkflowRunManager::CWorkflowRunManager(CWorkflowInputs *pInputs)
{
    m_pInputs = pInputs;
}

CWorkflowRunManager::~CWorkflowRunManager()
{
    // Ensure that thread will be stop when we close the software
    stopWaitThread();
    waitForWorkflow();
}

void CWorkflowRunManager::setManagers(CProjectManager *pProjectMgr, CMainDataManager* pDataMgr, CProgressBarManager *pProgressMgr)
{
    m_pProjectMgr = pProjectMgr;
    m_pDataMgr = pDataMgr;
    m_pProgressMgr = pProgressMgr;
    connect(this, &CWorkflowRunManager::doAbortProgressBar, m_pProgressMgr, &CProgressBarManager::onAbortProgressBar);
}

void CWorkflowRunManager::setWorkflow(WorkflowPtr WorkflowPtr)
{
    m_workflowPtr = WorkflowPtr;
    if(m_workflowPtr)
    {
        auto pWorkflowSignal = static_cast<CWorkflowSignalHandler*>(m_workflowPtr->getSignalRawPtr());
        connect(pWorkflowSignal, &CWorkflowSignalHandler::doSetElapsedTime, this, &CWorkflowRunManager::onSetElapsedTime);
        connect(pWorkflowSignal, &CWorkflowSignalHandler::doFinishWorkflow, this, &CWorkflowRunManager::onWorkflowFinished);
    }
}

double CWorkflowRunManager::getTotalElapsedTime() const
{
    return m_totalElapsedTime;
}

bool CWorkflowRunManager::isRunning() const
{
    return m_bRunning;
}

WorkflowTaskIOPtr CWorkflowRunManager::createTaskIO(size_t inputIndex, size_t dataIndex, bool bNewSequence)
{
    assert(m_pProjectMgr);
    assert(m_pDataMgr);

    TreeItemType type = m_pInputs->at(inputIndex).getType();
    if(type == TreeItemType::FOLDER)
    {
        size_t folderIndex = m_pInputs->at(inputIndex).getContainerIndex(dataIndex);
        if(folderIndex == SIZE_MAX)
            return nullptr;

        QModelIndex itemIndex = m_pInputs->at(inputIndex).getModelIndex(folderIndex);
        if(!itemIndex.isValid())
            return nullptr;

        auto types = getOriginTargetDataTypes(inputIndex);
        if(types.empty() || types.find(IODataType::PROJECT_FOLDER) != types.end() || types.find(IODataType::FOLDER_PATH) != types.end())
            return std::make_shared<CPathIO>(IODataType::PROJECT_FOLDER, m_pProjectMgr->getItemPath(itemIndex));
        else
        {
            size_t realDataIndex = m_pInputs->at(inputIndex).getDataIndexInContainer(folderIndex, dataIndex);
            if(realDataIndex == SIZE_MAX)
                return nullptr;

            QModelIndex dataModelIndex = m_pProjectMgr->getFolderDataIndex(itemIndex, realDataIndex);
            if(!dataModelIndex.isValid())
                return nullptr;

            return createIOFromDataItem(dataModelIndex, bNewSequence);
        }
    }
    else if(type == TreeItemType::DATASET)
    {
        size_t datasetIndex = m_pInputs->at(inputIndex).getContainerIndex(dataIndex);
        if(datasetIndex == SIZE_MAX)
            return nullptr;

        size_t realDataIndex = m_pInputs->at(inputIndex).getDataIndexInContainer(datasetIndex, dataIndex);
        if(realDataIndex == SIZE_MAX)
            return nullptr;

        QModelIndex datasetModelIndex = m_pInputs->at(inputIndex).getModelIndex(datasetIndex);
        if(!datasetModelIndex.isValid())
            return nullptr;

        QModelIndex dataModelIndex = m_pProjectMgr->getDatasetDataIndex(datasetModelIndex, realDataIndex);
        if(!dataModelIndex.isValid())
            return nullptr;

        return createIOFromDataItem(dataModelIndex, bNewSequence);
    }
    else if(type == TreeItemType::IMAGE || type == TreeItemType::VIDEO || type == TreeItemType::LIVE_STREAM)
    {
        QModelIndex itemIndex = m_pInputs->at(inputIndex).getModelIndex(dataIndex);
        if(!itemIndex.isValid())
            return nullptr;

        return createIOFromDataItem(itemIndex, bNewSequence);
    }
    else
        throw CException(CoreExCode::INVALID_USAGE, tr("Workflow inputs error : invalid item type.").toStdString(), __func__, __FILE__, __LINE__);

    return nullptr;
}

WorkflowTaskIOPtr CWorkflowRunManager::createIOFromDataItem(const QModelIndex &index, bool bNewSequence)
{
    if(!index.isValid())
        throw CException(CoreExCode::INVALID_USAGE, tr("Workflow inputs error : invalid item type.").toStdString(), __func__, __FILE__, __LINE__);

    std::string inputName;
    WorkflowTaskIOPtr inputPtr = nullptr;
    std::string itemPath = m_pProjectMgr->getItemPath(index);
    auto wrapInd = m_pProjectMgr->wrapIndex(index);
    auto pItem = static_cast<ProjectTreeItem*>(wrapInd.internalPointer());

    if(!pItem)
        throw CException(CoreExCode::INVALID_USAGE, tr("Workflow inputs error : invalid item type.").toStdString(), __func__, __FILE__, __LINE__);

    auto type =  static_cast<TreeItemType>(pItem->getTypeId());
    if(type == TreeItemType::IMAGE)
    {
        auto pDataset = CProjectUtils::getDataset<CMat>(wrapInd);
        if(pDataset->hasDimension(DataDimension::VOLUME))
        {
            // 3D images
            CMat image = m_pDataMgr->getImgMgr()->getImage(wrapInd);
            if(!image.data)
                throw CException(CoreExCode::INVALID_IMAGE, tr("Workflow inputs error : invalid volume.").toStdString(), __func__, __FILE__, __LINE__);

            inputName = pDataset->getInfo().getName();
            DimensionIndices indices = CProjectUtils::getIndicesInDataset(wrapInd);
            auto currentImgIndex = Utils::Data::getDimensionSize(indices, DataDimension::IMAGE);
            inputPtr = std::make_shared<CImageIO>(IODataType::VOLUME, image);
            std::static_pointer_cast<CImageIO>(inputPtr)->setCurrentImage(currentImgIndex);
        }
        else if(pDataset->hasDimension(DataDimension::TIME))
        {
            // Time image sequence
            auto videoImage = m_pDataMgr->getVideoMgr()->getCurrentImage(index);
            if(!videoImage.data)
                throw CException(CoreExCode::INVALID_IMAGE, tr("Workflow inputs error : invalid video frame.").toStdString(), __func__, __FILE__, __LINE__);

            inputName = pDataset->getInfo().getName();
            inputPtr = std::make_shared<CVideoIO>(IODataType::VIDEO, videoImage);
            inputPtr->setName(pDataset->getInfo().getName());
            std::static_pointer_cast<CVideoIO>(inputPtr)->setVideoPath(m_pProjectMgr->getItemPath(index));
        }
        else
        {
            // Single 2D image
            CMat image = m_pDataMgr->getImgMgr()->getImage(wrapInd);
            if(!image.data)
                throw CException(CoreExCode::INVALID_IMAGE, tr("Workflow inputs error : invalid image.").toStdString(), __func__, __FILE__, __LINE__);

            inputName = pItem->getName();
            inputPtr = std::make_shared<CImageIO>(IODataType::IMAGE, image);
        }
    }
    else if(type == TreeItemType::VIDEO)
    {
        // Video
        auto image = m_pDataMgr->getVideoMgr()->getCurrentImage(index);
        if(!image.data)
            throw CException(CoreExCode::INVALID_IMAGE, tr("Workflow inputs error : invalid video.").toStdString(), __func__, __FILE__, __LINE__);

        inputName = pItem->getName();
        inputPtr = std::make_shared<CVideoIO>(IODataType::VIDEO, image);

        if(bNewSequence)
        {
            auto inputVideoPtr = std::static_pointer_cast<CVideoIO>(inputPtr);
            inputVideoPtr->setVideoPath(m_pProjectMgr->getItemPath(index));
        }
    }
    else if(type == TreeItemType::LIVE_STREAM)
    {
        // Streaming
        auto image = m_pDataMgr->getVideoMgr()->getCurrentImage(index);
        if(!image.data)
            throw CException(CoreExCode::INVALID_IMAGE, tr("Workflow inputs error : invalid camera stream.").toStdString(), __func__, __FILE__, __LINE__);

        inputName = pItem->getName();
        inputPtr = std::make_shared<CVideoIO>(IODataType::LIVE_STREAM, image);
    }
    else
        throw CException(CoreExCode::INVALID_USAGE, tr("Workflow inputs error : invalid item type.").toStdString(), __func__, __FILE__, __LINE__);

    if(inputPtr)
    {
        inputPtr->setName(inputName);
        CDataInfoPtr infoPtr = inputPtr->getDataInfo();

        if(infoPtr)
            infoPtr->setFileName(itemPath);
    }
    return inputPtr;
}

void CWorkflowRunManager::run()
{
    if(m_bRunning)
    {
        qCWarning(logWorkflow).noquote() << "Workflow is already running...";
        return;
    }

    m_bRunning = true;
    m_bStop = false;

    if(m_workflowPtr->isBatchMode())
        runBatch();
    else
        runSingle();
}

void CWorkflowRunManager::runLive(size_t inputIndex)
{
    if(m_workflowPtr == nullptr)
        return;

    if(!checkLiveInputs())
        throw CException(CoreExCode::INVALID_USAGE, tr("Invalid input: workflow can only manage one video or stream input for now.").toStdString(), __func__, __FILE__, __LINE__);

    if(m_liveWatcher.isFinished())
    {
        m_liveInputIndex = inputIndex;
        m_bStopThread = false;

        auto protocolThread = QtConcurrent::run([&]
        {
            try
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_workflowPtr->workflowStarted();

                while(!m_bStopThread)
                {
                    if(m_bRunning == false)
                    {
                        m_bRunning = true;
                        m_totalElapsedTime = 0;
                        CPyEnsureGIL gil;
                        m_workflowPtr->run();
                    }

                    if(m_bStopThread)
                        break;

                    m_threadCond.wait(lock);
                }
            }
            catch(std::exception& e)
            {
                protocolErrorHandling(e);
            }
        });
        m_liveWatcher.setFuture(protocolThread);
    }
    else
        m_threadCond.notify_one();
}

void CWorkflowRunManager::runFromActiveTask()
{
    if(m_bRunning)
    {
        qCWarning(logWorkflow).noquote() << "Workflow is already running...";
        return;
    }

    m_bRunning = true;
    m_bStop = false;

    if(m_workflowPtr->isBatchMode())
        runFromBatch();
    else
        runFromSingle();
}

void CWorkflowRunManager::runToActiveTask()
{
    if(m_bRunning)
    {
        qCWarning(logWorkflow).noquote() << "Workflow is already running...";
        return;
    }

    m_bRunning = true;
    m_bStop = false;

    // Check if root and if so, don't do anything
    auto taskId = m_workflowPtr->getActiveTaskId();
    if(m_workflowPtr->isRoot(taskId))
        return;

     if(m_workflowPtr->isBatchMode())
         runToBatch();
     else
         runToSingle();
}

void CWorkflowRunManager::runSequentialTask(const WorkflowVertex &taskId)
{
    if(m_sequentialRuns.empty())
        return;

    auto nextTaskId = m_sequentialRuns.front();
    if(nextTaskId != taskId)
        return;

    if(m_bRunning)
    {
        qCWarning(logWorkflow).noquote() << tr("Workflow is already running...");
        return;
    }

    m_bRunning = true;
    //Thread watcher -> protocol manager
    m_pSequentialRunWatcher = new QFutureWatcher<void>;
    connect(m_pSequentialRunWatcher, &QFutureWatcher<void>::finished, this, &CWorkflowRunManager::onSequentialRunFinished);

    auto taskPtr = m_workflowPtr->getTask(taskId);
    auto pSignal = static_cast<CWorkflowSignalHandler*>(m_workflowPtr->getSignalRawPtr());
    m_pProgressMgr->launchProgress(pSignal, QString("The process %1 is running.").arg(QString::fromStdString(taskPtr->getName())), true);
    pSignal->emitSetTotalSteps(m_workflowPtr->getProgressSteps());

    auto future = QtConcurrent::run([this, taskId]
    {
        try
        {
            //Run process
            m_workflowPtr->workflowStarted();
            m_totalElapsedTime = 0;
            m_workflowPtr->runTo(taskId);
        }
        catch(std::exception& e)
        {
            m_pSequentialRunWatcher->disconnect();
            m_sequentialRuns.clear();
            protocolErrorHandling(e);
        }
    });
    m_pSequentialRunWatcher->setFuture(future);
    m_sync.setFuture(future);
}

void CWorkflowRunManager::addSequentialRun(const WorkflowVertex& taskId)
{
    m_sequentialRuns.push_back(taskId);
}

void CWorkflowRunManager::notifyGraphicsChanged()
{
    if(m_workflowPtr == nullptr)
        return;

    auto taskPtr = m_workflowPtr->getTask(m_workflowPtr->getActiveTaskId());
    if(taskPtr->isGraphicsChangedListening() == false)
        return;

    auto pSignal = static_cast<CWorkflowSignalHandler*>(m_workflowPtr->getSignalRawPtr());
    m_pProgressMgr->launchProgress(pSignal, QString("The workflow %1 is running.").arg(QString::fromStdString(m_workflowPtr->getName())), true);

    auto future = QtConcurrent::run([&]
    {
        try
        {
            m_workflowPtr->notifyGraphicsChanged();
        }
        catch(std::exception& e)
        {
            protocolErrorHandling(e);
        }
    });
    m_sync.setFuture(future);
}

void CWorkflowRunManager::stop()
{
    if(m_workflowPtr && isRunning())
    {
        try
        {
            m_bStop = true;
            m_workflowPtr->stop();
        }
        catch(const std::exception& e)
        {
            qCritical(logWorkflow) << QString::fromStdString(e.what());
        }
    }
}

void CWorkflowRunManager::stopWaitThread()
{
    m_bStopThread = true;
    m_threadCond.notify_all();
}

void CWorkflowRunManager::manageWaitThread(bool bNewSequence)
{
    if(m_waitThreadWatcher.isFinished())
    {
        m_bStopThread = false;

        auto waitThread = QtConcurrent::run([this, bNewSequence]
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            while(!m_bStopThread)
            {
                if(m_bStopThread)
                    break;

                waitForWorkflow();

                if(m_bStopThread)
                    break;

                // Emit signal for processing new image
                emit doWorkflowLive(m_liveInputIndex, bNewSequence);

                m_threadCond.wait(lock);
            }
        });
        m_waitThreadWatcher.setFuture(waitThread);
    }
    else
        m_threadCond.notify_one();
}

void CWorkflowRunManager::protocolErrorHandling(const std::exception &e)
{
    QString msg = "";
    try
    {
        auto cvErr = dynamic_cast<const cv::Exception&>(e);

#ifdef QT_DEBUG
        msg = QString::fromStdString(cvErr.what());
#else
        msg = QString::fromStdString(cvErr.err);
#endif
    }
    catch(...)
    {
        msg = QString(e.what());
    }
    m_bRunning = false;
    emit doAbortProgressBar();
    auto pWorkflowSignal = static_cast<CWorkflowSignalHandler*>(m_workflowPtr->getSignalRawPtr());
    emit pWorkflowSignal->doFinishTask(m_workflowPtr->getRunningTaskId(), CWorkflowTask::State::_ERROR, msg);
    qCCritical(logWorkflow).noquote() << msg;
}

void CWorkflowRunManager::batchErrorHandling(const std::exception &e)
{
    QString msg = "";
    try
    {
        auto cvErr = dynamic_cast<const cv::Exception&>(e);

#ifdef QT_DEBUG
        msg = QString::fromStdString(cvErr.what());
#else
        msg = QString::fromStdString(cvErr.err);
#endif
    }
    catch(...)
    {
        msg = QString(e.what());
    }
    qCCritical(logWorkflow).noquote() << msg;
    m_bStop = true;
}

void CWorkflowRunManager::onSetElapsedTime(double time)
{
    m_totalElapsedTime += time;
    emit doSetElapsedTime(time);
}

void CWorkflowRunManager::onSequentialRunFinished()
{
    delete m_pSequentialRunWatcher;
    m_pSequentialRunWatcher = nullptr;
    assert(m_sequentialRuns.size()>0);
    m_sequentialRuns.pop_front();

    if(m_sequentialRuns.empty() == false)
        runSequentialTask(m_sequentialRuns.front());
    else
        m_bRunning = false;
}

void CWorkflowRunManager::onWorkflowFinished()
{
    if(!m_workflowPtr->isBatchMode() || m_batchIndex == m_batchCount - 1 || m_bStop)
    {
        m_bRunning = false;
        m_workflowPtr->workflowFinished();
        emit doWorkflowFinished();
    }
}

void CWorkflowRunManager::setBatchInput(int index)
{
    for(size_t i=0; i<m_pInputs->size(); ++i)
    {
        auto inputPtr = createTaskIO(i, index, true);
        if(inputPtr)
            m_workflowPtr->setInput(inputPtr, i, true);
    }
}

std::set<IODataType> CWorkflowRunManager::getTargetDataTypes(size_t inputIndex) const
{
    assert(m_workflowPtr);
    std::set<IODataType> types;
    WorkflowVertex root = m_workflowPtr->getRootId();
    auto outEdges = m_workflowPtr->getOutEdges(root);

    for(auto it=outEdges.first; it!=outEdges.second; ++it)
    {
        WorkflowEdgePtr edge = m_workflowPtr->getEdge(*it);
        if(edge->getSourceIndex() == inputIndex)
        {
            WorkflowVertex target = m_workflowPtr->getEdgeTarget(*it);
            WorkflowTaskPtr targetTask = m_workflowPtr->getTask(target);
            IODataType type = targetTask->getInputDataType(edge->getTargetIndex());
            types.insert(type);
        }
    }
    return types;
}

size_t CWorkflowRunManager::getBatchCount() const
{
    assert(m_pProjectMgr);

    size_t count = 0;
    for(size_t i=0; i<m_pInputs->size(); ++i)
    {
        size_t inputCount = 0;
        if(m_pInputs->at(i).getType() == TreeItemType::DATASET)
        {
            for(size_t j=0; j<m_pInputs->at(i).getModelIndicesCount(); ++j)
            {
                QModelIndex itemIndex = m_pInputs->at(i).getModelIndex(j);
                auto wrapInd = m_pProjectMgr->wrapIndex(itemIndex);
                auto pDataset = CProjectUtils::getDataset<CMat>(wrapInd);

                if(pDataset->hasDimension(DataDimension::VOLUME) || pDataset->hasDimension(DataDimension::TIME))
                {
                    m_pInputs->at(i).setSize(j, 1);
                    inputCount++;
                }
                else
                {
                    m_pInputs->at(i).setSize(j, pDataset->size());
                    inputCount += pDataset->size();
                }
            }
        }
        else if(m_pInputs->at(i).getType() == TreeItemType::FOLDER)
        {
            for(size_t j=0; j<m_pInputs->at(i).getModelIndicesCount(); ++j)
            {
                QModelIndex itemIndex = m_pInputs->at(i).getModelIndex(j);
                size_t folderItemCount = m_pProjectMgr->getFolderDataItemCount(itemIndex);
                m_pInputs->at(i).setSize(j, folderItemCount);
                inputCount += folderItemCount;
            }
        }
        else if(m_pInputs->at(i).getMode() != WorkflowInputMode::CURRENT_DATA)
            inputCount = m_pInputs->at(i).getModelIndicesCount();
        else
            inputCount = 1;

        count = std::max(count, inputCount);
    }
    return count;
}

std::set<IODataType> CWorkflowRunManager::getOriginTargetDataTypes(size_t inputIndex) const
{
    assert(m_workflowPtr);
    std::set<IODataType> types;
    WorkflowVertex root = m_workflowPtr->getRootId();
    auto outEdges = m_workflowPtr->getOutEdges(root);

    for(auto it=outEdges.first; it!=outEdges.second; ++it)
    {
        WorkflowEdgePtr edge = m_workflowPtr->getEdge(*it);
        if(edge->getSourceIndex() == inputIndex)
        {
            WorkflowVertex target = m_workflowPtr->getEdgeTarget(*it);
            WorkflowTaskPtr targetTask = m_workflowPtr->getTask(target);
            IODataType type = targetTask->getOriginalInputDataType(edge->getTargetIndex());
            types.insert(type);
        }
    }
    return types;
}

bool CWorkflowRunManager::checkInputs(std::string& err) const
{
    // Get connected input indices
    std::set<size_t> connectedIndices;
    WorkflowVertex root = m_workflowPtr->getRootId();
    auto outEdges = m_workflowPtr->getOutEdges(root);

    for(auto it=outEdges.first; it!=outEdges.second; ++it)
    {
        WorkflowEdgePtr edge = m_workflowPtr->getEdge(*it);
        connectedIndices.insert(edge->getSourceIndex());
    }

    // Check the validity of inputs combination
    for(auto it1=connectedIndices.begin(); it1!=connectedIndices.end(); ++it1)
    {
        auto it2 = it1;
        for(++it2; it2!=connectedIndices.end(); ++it2)
        {
            if(!checkInputs(*it1, *it2, err))
                return false;
        }
    }
    return true;
}

bool CWorkflowRunManager::checkInputs(size_t index1, size_t index2, std::string& err) const
{
    TreeItemType type1 = m_pInputs->at(index1).getType();
    TreeItemType type2 = m_pInputs->at(index1).getType();
    err = "Incompatibility detected between inputs #" + std::to_string(index1 + 1) + " and " + std::to_string(index2 + 1) + ": ";

    if(type1 == TreeItemType::IMAGE && type2 == TreeItemType::IMAGE)
        return checkImageInputs(index1, index2, err);
    else if(type1 == TreeItemType::IMAGE && type2 == TreeItemType::VIDEO)
        return checkImageVideoInputs(index1, index2, err);
    else if(type1 == TreeItemType::VIDEO && type2 == TreeItemType::IMAGE)
        return checkImageVideoInputs(index2, index1, err);
    else if(type1 == TreeItemType::IMAGE && type2 == TreeItemType::DATASET)
        return checkImageDatasetInputs(index1, index2, err);
    else if(type1 == TreeItemType::DATASET && type2 == TreeItemType::IMAGE)
        return checkImageDatasetInputs(index2, index1, err);
    else if(type1 == TreeItemType::IMAGE && type2 == TreeItemType::FOLDER)
        return checkImageFolderInputs(index1, index2, err);
    else if(type1 == TreeItemType::FOLDER && type2 == TreeItemType::IMAGE)
        return checkImageFolderInputs(index2, index1, err);
    else if(type1 == TreeItemType::VIDEO && type2 == TreeItemType::VIDEO)
        return checkVideoInputs(index1, index2, err);
    else if(type1 == TreeItemType::VIDEO && type2 == TreeItemType::DATASET)
        return checkVideoDatasetInputs(index1, index2, err);
    else if(type1 == TreeItemType::DATASET && type2 == TreeItemType::VIDEO)
        return checkVideoDatasetInputs(index2, index1, err);
    else if(type1 == TreeItemType::VIDEO && type2 == TreeItemType::FOLDER)
        return checkVideoFolderInputs(index1, index2, err);
    else if(type1 == TreeItemType::FOLDER && type2 == TreeItemType::VIDEO)
        return checkVideoFolderInputs(index2, index1, err);
    else if(type1 == TreeItemType::DATASET && type2 == TreeItemType::DATASET)
        return checkDatasetInputs(index2, index1, err);
    else if(type1 == TreeItemType::DATASET && type2 == TreeItemType::FOLDER)
        return checkDatasetFolderInputs(index1, index2, err);
    else if(type1 == TreeItemType::FOLDER && type2 == TreeItemType::DATASET)
        return checkDatasetFolderInputs(index2, index1, err);
    else if(type1 == TreeItemType::FOLDER && type2 == TreeItemType::FOLDER)
        return checkFolderInputs(index2, index1, err);
    else if(type1 == TreeItemType::LIVE_STREAM || type2 == TreeItemType::LIVE_STREAM)
    {
        err += "batch process is not possible with camera stream.";
        return false;
    }
    else
    {
        err += "input mix not supported.";
        return false;
    }
}

bool CWorkflowRunManager::checkLiveInputs() const
{
    // Get connected input indices
    std::set<size_t> connectedIndices;
    WorkflowVertex root = m_workflowPtr->getRootId();
    auto outEdges = m_workflowPtr->getOutEdges(root);

    for(auto it=outEdges.first; it!=outEdges.second; ++it)
    {
        WorkflowEdgePtr edge = m_workflowPtr->getEdge(*it);
        connectedIndices.insert(edge->getSourceIndex());
    }

    // Check the validity of inputs combination
    int liveInputCount = 0;
    for(auto it1=connectedIndices.begin(); it1!=connectedIndices.end(); ++it1)
    {
        TreeItemType type = m_pInputs->at(*it1).getType();
        if(type == TreeItemType::VIDEO || type == TreeItemType::LIVE_STREAM)
            liveInputCount++;
    }
    return liveInputCount <= 1;
}

bool CWorkflowRunManager::checkImageInputs(size_t index1, size_t index2, std::string &err) const
{
    size_t nb1 = m_pInputs->at(index1).getModelIndicesCount();
    size_t nb2 = m_pInputs->at(index2).getModelIndicesCount();
    QModelIndex itemIndex1 = m_pInputs->at(index1).getModelIndex(0);
    QModelIndex itemIndex2 = m_pInputs->at(index2).getModelIndex(0);
    auto pDataset1 = CProjectUtils::getDataset<CMat>(m_pProjectMgr->wrapIndex(itemIndex1));
    auto pDataset2 = CProjectUtils::getDataset<CMat>(m_pProjectMgr->wrapIndex(itemIndex2));

    if(pDataset1->hasDimension(DataDimension::VOLUME) && pDataset2->hasDimension(DataDimension::VOLUME))
    {
        // VOLUME - VOLUME
        if(pDataset1->getDataInfo().dimensions() != pDataset2->getDataInfo().dimensions())
        {
            err += "volumes must have same dimensions.";
            return false;
        }
        else if(nb1 != nb2)
        {
            err += "volumes count is different.";
            return false;
        }
        else
            return true;
    }
    else if((pDataset1->hasDimension(DataDimension::VOLUME) && pDataset2->hasDimension(DataDimension::TIME)) ||
            (pDataset1->hasDimension(DataDimension::TIME) && pDataset2->hasDimension(DataDimension::VOLUME)))
    {
        // VOLUME - TIME
        // TIME - VOLUME
        err += "mix volume and time-serie is not possible.";
        return false;
    }
    else if(pDataset1->hasDimension(DataDimension::VOLUME))
    {
        // VOLUME - IMAGE
        bool bValid = nb2 == 1;
        if(!bValid)
            err += "mix volume and single image is only possible if image count is 1.";

        return bValid;
    }
    else if(pDataset2->hasDimension(DataDimension::VOLUME))
    {
        // IMAGE - VOLUME
        bool bValid = nb1 == 1;
        if(!bValid)
            err += "mix volume and single image is only possible if image count is 1.";

        return bValid;
    }
    else if(pDataset1->hasDimension(DataDimension::TIME) && pDataset2->hasDimension(DataDimension::TIME))
    {
        // TIME_SERIE - TIME_SERIE
        if(pDataset1->getDataInfo().dimensions() != pDataset2->getDataInfo().dimensions())
        {
            err += "time-series must have same dimensions.";
            return false;
        }
        else if(nb1 != nb2)
        {
            err += "time-series count is different.";
            return false;
        }
        else
            return true;
    }
    else if(pDataset1->hasDimension(DataDimension::TIME))
    {
        // TIME_SERIE - IMAGE
        bool bValid = nb2 == 1;
        if(!bValid)
            err += "mix time-serie and single image is only possible if image count is 1.";

        return bValid;
    }
    else if(pDataset2->hasDimension(DataDimension::TIME))
    {
        // IMAGE - TIME_SERIE
        bool bValid = nb1 == 1;
        if(!bValid)
            err += "mix time-serie and single image is only possible if image count is 1.";

        return bValid;
    }
    else
    {
        // IMAGE - IMAGE
        bool bValid = (nb1 == nb2 || nb1 == 1 || nb2 == 1);
        if(!bValid)
            err += "images count doesn't match.";

        return bValid;
    }
}

bool CWorkflowRunManager::checkImageVideoInputs(size_t index1, size_t index2, std::string &err) const
{
    Q_UNUSED(index2);
    QModelIndex itemIndex = m_pInputs->at(index1).getModelIndex(0);
    auto pDataset = CProjectUtils::getDataset<CMat>(m_pProjectMgr->wrapIndex(itemIndex));

    if(pDataset->hasDimension(DataDimension::VOLUME))
    {
        // VOLUME - VIDEO
        err += "mix volume and video is not possible.";
        return false;
    }
    else if(pDataset->hasDimension(DataDimension::TIME))
    {
        // TIME_SERIE - VIDEO
        err += "mix time-serie and video is not possible.";
        return false;
    }
    else
    {
        // IMAGE - VIDEO
        bool bValid = m_pInputs->at(index1).getModelIndicesCount() == 1;
        if(!bValid)
            err += "mix video and single image is only possible if image count is 1.";

        return bValid;
    }
}

bool CWorkflowRunManager::checkImageDatasetInputs(size_t index1, size_t index2, std::string &err) const
{
    size_t nb1 = m_pInputs->at(index1).getModelIndicesCount();
    size_t nb2 = m_pInputs->at(index2).getModelIndicesCount();
    QModelIndex itemIndex1 = m_pInputs->at(index1).getModelIndex(0);
    auto pDataset1 = CProjectUtils::getDataset<CMat>(m_pProjectMgr->wrapIndex(itemIndex1));

    if(pDataset1->hasDimension(DataDimension::VOLUME))
    {
        // VOLUME - DATASET
        if(nb2 != 1)
        {
            err += "only one dataset can be processed with single volume.";
            return false;
        }
        else
        {
            QModelIndex itemIndex2 = m_pInputs->at(index2).getModelIndex(0);
            auto pDataset2 = CProjectUtils::getDataset<CMat>(m_pProjectMgr->wrapIndex(itemIndex2));

            if(pDataset2->hasDimension(DataDimension::VOLUME))
            {
                if(pDataset1->getDataInfo().dimensions() != pDataset2->getDataInfo().dimensions())
                {
                    err += "volumes must have same dimensions.";
                    return false;
                }
                else
                    return true;
            }
            else
            {
                err += "dataset is not a volume.";
                return false;
            }
        }

    }
    else if(pDataset1->hasDimension(DataDimension::TIME))
    {
        // TIME_SERIE - DATASET
        if(nb2 != 1)
        {
            err += "only one dataset can be processed with single time-serie.";
            return false;
        }
        else
        {
            // Check if dataset is a time-serie
            QModelIndex itemIndex2 = m_pInputs->at(index2).getModelIndex(0);
            auto pDataset2 = CProjectUtils::getDataset<CMat>(m_pProjectMgr->wrapIndex(itemIndex2));
            QModelIndex dataItemIndex = m_pProjectMgr->getDatasetDataIndex(itemIndex2, 0);
            auto pItem = static_cast<ProjectTreeItem*>(m_pProjectMgr->wrapIndex(dataItemIndex).internalPointer());
            auto type =  static_cast<TreeItemType>(pItem->getTypeId());

            if(type != TreeItemType::IMAGE)
            {
                err += "dataset is not a time-serie.";
                return false;
            }
            else if(pDataset1->getDataInfo().dimensions() != pDataset2->getDataInfo().dimensions())
            {
                err += "time-series must have same dimensions.";
                return false;
            }
            else
                return true;
        }
    }
    else
    {
        // IMAGE - DATASET
        bool bValid = nb1 == 1;
        if(!bValid)
            err += "mix dataset and single image is only possible if image count is 1.";

        return bValid;
    }
}

bool CWorkflowRunManager::checkImageFolderInputs(size_t index1, size_t index2, std::string &err) const
{
    Q_UNUSED(index2)
    QModelIndex itemIndex1 = m_pInputs->at(index1).getModelIndex(0);
    auto pDataset1 = CProjectUtils::getDataset<CMat>(m_pProjectMgr->wrapIndex(itemIndex1));

    if(pDataset1->hasDimension(DataDimension::VOLUME))
    {
        err += "mix volume and folder is not possible.";
        return false;
    }
    else if(pDataset1->hasDimension(DataDimension::TIME))
    {
        err += "mix time-serie and folder is not possible.";
        return false;
    }
    else
    {
        size_t nb1 = m_pInputs->at(index1).getModelIndicesCount();
        bool bValid = nb1 == 1;

        if(!bValid)
            err += "mix folder and single image is only possible if image count is 1.";

        return bValid;
    }
}

bool CWorkflowRunManager::checkVideoInputs(size_t index1, size_t index2, std::string &err) const
{
    // VIDEO - VIDEO
    // Test on frames count can't be done here, must be done after loading videos
    bool bValid = m_pInputs->at(index1).getModelIndicesCount() == m_pInputs->at(index2).getModelIndicesCount();
    if(!bValid)
        err += "videos count is different.";

    return bValid;
}

bool CWorkflowRunManager::checkVideoDatasetInputs(size_t index1, size_t index2, std::string &err) const
{
    size_t nb1 = m_pInputs->at(index1).getModelIndicesCount();
    size_t nb2 = m_pInputs->at(index2).getModelIndicesCount();

    if(nb1 != nb2)
    {
        err += "video count is different.";
        return false;
    }
    else
    {
        for(size_t i=0; i<nb2; ++i)
        {
            QModelIndex itemIndex2 = m_pInputs->at(index2).getModelIndex(i);
            QModelIndex dataItemIndex = m_pProjectMgr->getDatasetDataIndex(itemIndex2, 0);
            auto pItem = static_cast<ProjectTreeItem*>(m_pProjectMgr->wrapIndex(dataItemIndex).internalPointer());
            auto type =  static_cast<TreeItemType>(pItem->getTypeId());

            // Test on frames count can't be done here, must be done after loading videos
            if(type != TreeItemType::VIDEO)
            {
                err += "dataset #" + std::to_string(i+1) + "(" + pItem->getName() + ") is not a video.";
                return false;
            }
        }
        return true;
    }
}

bool CWorkflowRunManager::checkVideoFolderInputs(size_t index1, size_t index2, std::string &err) const
{
    Q_UNUSED(index1);
    Q_UNUSED(index2);
    err += "mix video(s) and folder(s)) is not possible.";
    return false;
}

bool CWorkflowRunManager::checkDatasetInputs(size_t index1, size_t index2, std::string &err) const
{
    size_t nb1 = m_pInputs->at(index1).getModelIndicesCount();
    size_t nb2 = m_pInputs->at(index2).getModelIndicesCount();

    if(nb1 != nb2)
    {
        err += "dataset count is different.";
        return false;
    }
    else
    {
        for(size_t i=0; i<nb1; ++i)
        {
            QModelIndex itemIndex1 = m_pInputs->at(index1).getModelIndex(i);
            QModelIndex itemIndex2 = m_pInputs->at(index2).getModelIndex(i);
            QModelIndex wrapIndex1 = m_pProjectMgr->wrapIndex(itemIndex1);
            QModelIndex wrapIndex2 = m_pProjectMgr->wrapIndex(itemIndex2);
            auto pItem1 = static_cast<ProjectTreeItem*>(wrapIndex1.internalPointer());
            auto pItem2 = static_cast<ProjectTreeItem*>(wrapIndex2.internalPointer());
            auto pDataset1 = CProjectUtils::getDataset<CMat>(wrapIndex1);
            auto pDataset2 = CProjectUtils::getDataset<CMat>(wrapIndex2);

            if(pDataset1->getType() != pDataset2->getType())
            {
                err += "datasets #" + std::to_string(i+1) + " (" + pItem1->getName() + " and " + pItem2->getName() + ") have different types.";
                return false;
            }
            else if(pDataset1->getDataInfo().dimensions() != pDataset2->getDataInfo().dimensions())
            {
                err += "datasets #" + std::to_string(i+1) + " (" + pItem1->getName() + " and " + pItem2->getName() + ") have different dimensions.";
                return false;
            }
        }
        return true;
    }
}

bool CWorkflowRunManager::checkDatasetFolderInputs(size_t index1, size_t index2, std::string &err) const
{
    Q_UNUSED(index1);
    Q_UNUSED(index2);
    err += "mix dataset(s) and folder(s)) is not possible.";
    return false;
}

bool CWorkflowRunManager::checkFolderInputs(size_t index1, size_t index2, std::string &err) const
{
    size_t nb1 = m_pInputs->at(index1).getModelIndicesCount();
    size_t nb2 = m_pInputs->at(index2).getModelIndicesCount();

    if(nb1 != nb2)
    {
        err += "folder count is different.";
        return false;
    }
    else
    {
        for(size_t i=0; i<nb1; ++i)
        {
            QModelIndex itemIndex1 = m_pInputs->at(index1).getModelIndex(i);
            QModelIndex itemIndex2 = m_pInputs->at(index1).getModelIndex(i);
            size_t folderItemCount1 = m_pProjectMgr->getFolderDataItemCount(itemIndex1);
            size_t folderItemCount2 = m_pProjectMgr->getFolderDataItemCount(itemIndex2);

            if(folderItemCount1 != folderItemCount2)
            {
                auto pItem1 = static_cast<ProjectTreeItem*>(m_pProjectMgr->wrapIndex(itemIndex1).internalPointer());
                auto pItem2 = static_cast<ProjectTreeItem*>(m_pProjectMgr->wrapIndex(itemIndex2).internalPointer());
                err += "folders #" + std::to_string(i+1) + " (" + pItem1->getName() + "and " + pItem2->getName() + ") have different items count.";
                return false;
            }
        }
        return true;
    }
}

void CWorkflowRunManager::runBatch()
{
    std::string erroMsg;
    if(!checkInputs(erroMsg))
    {
        m_bRunning = false;
        qCCritical(logWorkflow) << QString::fromStdString(erroMsg);
        return;
    }

    m_batchCount = getBatchCount();
    if(m_batchCount == 0)
    {
        m_bRunning = false;
        return;
    }

    auto pSignal = static_cast<CWorkflowSignalHandler*>(m_workflowPtr->getSignalRawPtr());
    m_pProgressMgr->launchProgress(pSignal, QString("The workflow %1 is running.").arg(QString::fromStdString(m_workflowPtr->getName())), true);
    pSignal->emitSetTotalSteps(m_batchCount * m_workflowPtr->getProgressSteps());

    auto future = QtConcurrent::run([&]
    {
        m_batchIndex = 0;
        m_totalElapsedTime = 0;
        m_workflowPtr->updateStartTime();
        m_workflowPtr->workflowStarted();

        for(size_t i=0; i<m_batchCount && !m_bStop; ++i)
        {
            try
            {
                setBatchInput(i);
                m_batchIndex = i;
                m_workflowPtr->clearAllOutputData();
                m_workflowPtr->run();
            }
            catch(std::exception& e)
            {
                batchErrorHandling(e);
            }
        }

        if(m_bStop)
            onWorkflowFinished();
    });
    m_processWatcher.setFuture(future);
    m_sync.setFuture(future);
}

void CWorkflowRunManager::runFromBatch()
{
    std::string errorMsg;
    if(!checkInputs(errorMsg))
    {
        m_bRunning = false;
        qCCritical(logWorkflow) << QString::fromStdString(errorMsg);
        return;
    }

    m_batchCount = getBatchCount();
    if(m_batchCount == 0)
    {
        m_bRunning = false;
        return;
    }

    auto pSignal = static_cast<CWorkflowSignalHandler*>(m_workflowPtr->getSignalRawPtr());
    m_pProgressMgr->launchProgress(pSignal, QString("The workflow %1 is running.").arg(QString::fromStdString(m_workflowPtr->getName())), true);
    pSignal->emitSetTotalSteps(m_batchCount * m_workflowPtr->getProgressStepsFrom(m_workflowPtr->getActiveTaskId()));

    auto future = QtConcurrent::run([&]
    {
        m_batchIndex = 0;
        m_totalElapsedTime = 0;
        auto id = m_workflowPtr->getActiveTaskId();
        auto taskPtr = m_workflowPtr->getTask(id);

        if(taskPtr == nullptr)
        {
            qCCritical(logWorkflow).noquote() << tr("Invalid workflow current task");
            return;
        }

        m_workflowPtr->workflowStarted();
        m_workflowPtr->updateStartTime();

        for(size_t i=0; i<m_batchCount && !m_bStop; ++i)
        {
            try
            {
                setBatchInput(i);
                m_batchIndex = i;
                m_workflowPtr->clearAllOutputData();
                m_workflowPtr->runFrom(id);
            }
            catch(std::exception& e)
            {
                batchErrorHandling(e);
            }
        }

        if(m_bStop)
            onWorkflowFinished();
    });
    m_processWatcher.setFuture(future);
    m_sync.setFuture(future);
}

void CWorkflowRunManager::runToBatch()
{
    std::string errorMsg;
    if(!checkInputs(errorMsg))
    {
        m_bRunning = false;
        qCCritical(logWorkflow) << QString::fromStdString(errorMsg);
        return;
    }

    m_batchCount = getBatchCount();
    if(m_batchCount == 0)
    {
        m_bRunning = false;
        return;
    }

    auto pSignal = static_cast<CWorkflowSignalHandler*>(m_workflowPtr->getSignalRawPtr());
    m_pProgressMgr->launchProgress(pSignal, QString("The workflow %1 is running.").arg(QString::fromStdString(m_workflowPtr->getName())), true);
    pSignal->emitSetTotalSteps(m_batchCount * m_workflowPtr->getProgressStepsTo(m_workflowPtr->getActiveTaskId()));

    auto future = QtConcurrent::run([&]
    {
        m_batchIndex = 0;
        m_totalElapsedTime = 0;
        auto id = m_workflowPtr->getActiveTaskId();
        auto taskPtr = m_workflowPtr->getTask(id);

        if(taskPtr == nullptr)
        {
            qCCritical(logWorkflow).noquote() << tr("Invalid workflow current task");
            return;
        }

        m_workflowPtr->workflowStarted();
        m_workflowPtr->updateStartTime();

        for(size_t i=0; i<m_batchCount && !m_bStop; ++i)
        {
            try
            {
                setBatchInput(i);
                m_batchIndex = i;
                m_workflowPtr->clearAllOutputData();
                m_workflowPtr->runTo(id);
            }
            catch(std::exception& e)
            {
                batchErrorHandling(e);
            }
        }

        if(m_bStop)
            onWorkflowFinished();
    });
    m_processWatcher.setFuture(future);
    m_sync.setFuture(future);
}

void CWorkflowRunManager::runSingle()
{    
    auto pSignal = static_cast<CWorkflowSignalHandler*>(m_workflowPtr->getSignalRawPtr());
    m_pProgressMgr->launchProgress(pSignal, QString("The workflow %1 is running.").arg(QString::fromStdString(m_workflowPtr->getName())), true);
    pSignal->emitSetTotalSteps(m_workflowPtr->getProgressSteps());

    auto future = QtConcurrent::run([&]
    {
        try
        {
            CPyEnsureGIL gil;
            m_workflowPtr->workflowStarted();
            m_totalElapsedTime = 0;
            m_workflowPtr->run();
        }
        catch(std::exception& e)
        {
            protocolErrorHandling(e);
        }
    });
    m_processWatcher.setFuture(future);
    m_sync.setFuture(future);
}

void CWorkflowRunManager::runFromSingle()
{
    auto pSignal = static_cast<CWorkflowSignalHandler*>(m_workflowPtr->getSignalRawPtr());
    m_pProgressMgr->launchProgress(pSignal, QString("The workflow %1 is running.").arg(QString::fromStdString(m_workflowPtr->getName())), true);
    pSignal->emitSetTotalSteps(m_workflowPtr->getProgressStepsFrom(m_workflowPtr->getActiveTaskId()));

    auto future = QtConcurrent::run([this]
    {
        auto id = m_workflowPtr->getActiveTaskId();
        auto pTask = m_workflowPtr->getTask(id);

        if(pTask == nullptr)
        {
            qCCritical(logWorkflow).noquote() << tr("Invalid workflow current task");
            return;
        }

        try
        {
            CPyEnsureGIL gil;
            m_workflowPtr->workflowStarted();
            m_totalElapsedTime = 0;
            m_workflowPtr->runFrom(id);
        }
        catch(std::exception& e)
        {
            protocolErrorHandling(e);
        }
    });
    m_sync.setFuture(future);
}

void CWorkflowRunManager::runToSingle()
{
    auto pSignal = static_cast<CWorkflowSignalHandler*>(m_workflowPtr->getSignalRawPtr());
    m_pProgressMgr->launchProgress(pSignal, QString("The workflow %1 is running.").arg(QString::fromStdString(m_workflowPtr->getName())), true);
    pSignal->emitSetTotalSteps(m_workflowPtr->getProgressStepsTo(m_workflowPtr->getActiveTaskId()));

    auto future = QtConcurrent::run([this]
    {
        auto id = m_workflowPtr->getActiveTaskId();
        auto pTask = m_workflowPtr->getTask(id);

        if(pTask == nullptr)
        {
            qCCritical(logWorkflow).noquote() << tr("Invalid workflow current task");
            return;
        }

        try
        {
            m_workflowPtr->workflowStarted();
            m_totalElapsedTime = 0;
            m_workflowPtr->runTo(id);
        }
        catch(std::exception& e)
        {
            protocolErrorHandling(e);
        }
    });
    m_sync.setFuture(future);
}

void CWorkflowRunManager::waitForWorkflow()
{
    if(m_bRunning)
    {
        // We must wait for the pending protocol
        m_sync.waitForFinished();

        // When finished, onFinishedWorkflow is called only after this function so
        // we have to set m_bWorkflowRunning to false manually otherwise, runWorkflowLive
        // won't work
        m_bRunning = false;
    }
}
